/* Load dgVoodoo as ddraw_eeorig.dll and fake 800x600 display modes at process start. */
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

/* What we report when the wrapper says the card has no video memory. */
#define EE_FAKE_VIDMEM (256u * 1024u * 1024u)

static HMODULE g_real;
static FILE *g_log;
static int(WINAPI *orig_GetSystemMetrics)(int);
static void ee_log(const char *fmt, ...);
static void drop_topmost_excl(const char *why, BOOL async);
static void pages_release(const char *why);

/* ---- hot-path log throttling ---------------------------------------------
 * Once the game actually renders, the proxy wrote 16 MB in five and a half
 * minutes -- 23,917 Device7::SetViewport lines alone -- and ee_log is
 * unbuffered, so every one of those is a synchronous write in the middle of a
 * frame.  Keep the first few of each event (which is all the diagnosis ever
 * needed) plus a periodic heartbeat, and let EE_DDRAW_VERBOSE=1 restore the
 * old firehose. */
static int ee_verbose(void) {
  static int v = -1;
  if (v < 0) {
    char b[8];
    v = (GetEnvironmentVariableA("EE_DDRAW_VERBOSE", b, sizeof b) > 0 && b[0] == '1') ? 1 : 0;
  }
  return v;
}

/* Throttling made the loading phase invisible -- thousands of texture surfaces
 * and locks now go unlogged, so a healthy load looks identical to a stall.
 * These counters are printed by the watchdog heartbeat instead. */
static volatile LONG g_n_surfaces, g_n_locks, g_n_frames, g_n_blts, g_n_bltfail;
/* Where a frame's time goes (heartbeat dc=/rdc=/flip=): microseconds inside
 * the real GetDC and ReleaseDC on the back buffer (D7VK downloads the frame
 * for GDI text and uploads it again) and inside the real Flip. */
static volatile LONG g_dc_us, g_dc_n, g_rdc_us, g_rdc_n, g_flip_us, g_flip_n, g_n_bltfix;
/* Frame pacing (heartbeat ft=): intervals between BeginScenes in ms buckets
 * <8, 8-11, 11-14, 14-17, 17-20, 20-25, 25-33, 33-50, 50+, and the longest.
 * The fps average hides the frames that wait out a whole simulation tick. */
static volatile LONG g_ft[9], g_ft_max;
static LONG us_since(const LARGE_INTEGER *t0) {
  static LARGE_INTEGER f;
  LARGE_INTEGER t1;
  if (!f.QuadPart)
    QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&t1);
  return (LONG)((t1.QuadPart - t0->QuadPart) * 1000000 / f.QuadPart);
}

/* Page exchange state (see pages_prepare). */
static CRITICAL_SECTION g_pages_cs;                   /* initialised in DllMain */
static volatile LONG g_drew_3d;                       /* a Direct3D draw call since the last flip */
static volatile LONG g_locked_back;                   /* the game locked the back buffer since then */
static volatile LONG g_draws, g_dcs;                  /* draw calls / GetDCs since the last flip (log) */
static volatile LONG g_flips_since_chain;             /* flips since the spare page was made (log) */
static IDirectDrawSurface7 *volatile g_pages_primary; /* primary being exchanged (not owned) */
static volatile LONG g_pages_n, g_pages_us;           /* exchanged flips and time spent */
/* The flip chain the game draws into (from its GetAttachedSurface call; not
 * owned, compared only) -- see soft_blt. */
static IDirectDrawSurface7 *volatile g_chain_front, *volatile g_chain_back;
static volatile LONG g_soft_n, g_soft_fallback;
static volatile LONG g_nvbs, g_vb_draws, g_vb_overruns, g_vb_maxverts; /* see vb_check */

static int hot_ok(unsigned *n) {
  unsigned c = (*n)++;
  if (ee_verbose())
    return 1;
  return c < 12 || (c % 20000) == 0;
}

#ifndef DDERR_TESTFINISHED
#define DDERR_TESTFINISHED 0x8876010D
#endif

static HRESULT(STDMETHODCALLTYPE *orig_CreateSurface7)(IDirectDraw7 *, DDSURFACEDESC2 *, IDirectDrawSurface7 **, IUnknown *);
static HRESULT(STDMETHODCALLTYPE *orig_EnumDisplayModes7)(IDirectDraw7 *, DWORD, DDSURFACEDESC2 *, void *,
                                                          LPDDENUMMODESCALLBACK2);
static HRESULT(STDMETHODCALLTYPE *orig_GetCaps7)(IDirectDraw7 *, DDCAPS *, DDCAPS *);
static HRESULT(STDMETHODCALLTYPE *orig_GetDisplayMode7)(IDirectDraw7 *, DDSURFACEDESC2 *);
static HRESULT(STDMETHODCALLTYPE *orig_SetCoop7)(IDirectDraw7 *, HWND, DWORD);
static HRESULT(STDMETHODCALLTYPE *orig_SetDisplayMode7)(IDirectDraw7 *, DWORD, DWORD, DWORD, DWORD, DWORD);
static HRESULT(STDMETHODCALLTYPE *orig_StartModeTest7)(IDirectDraw7 *, LPSIZE, DWORD, DWORD);
static HRESULT(STDMETHODCALLTYPE *orig_EvaluateMode7)(IDirectDraw7 *, DWORD, DWORD *);
static int g_mode_test_left;
static void wrap_surf7(void *obj);
static void fill_dd_mode(DDSURFACEDESC2 *sd, DWORD w, DWORD h, DWORD bpp);
static const char *guid_str(const GUID *g);
/* The display mode the game believes it set (SetDisplayMode remembers it so
 * GetDisplayMode can agree). */
static DWORD g_mode_w, g_mode_h, g_mode_bpp;
static LONG g_mode_frame0; /* g_n_frames when the mode last changed */
static void wrap_d3ddev7(void *obj);

/* D7VK leaves some COM methods unimplemented; the game calls one and jumps to
 * address 0. Report which slots are NULL so the offender can be named. */
static void log_null_slots(const char *what, void **vt, unsigned slots) {
  unsigned i;
  char line[512];
  int n = 0;
  line[0] = 0;
  for (i = 0; i < slots; i++) {
    if (!vt[i]) {
      int w = wsprintfA(line + n, "%s%u", n ? "," : "", i);
      if (w > 0)
        n += w;
      if (n > 400)
        break;
    }
  }
  if (n)
    ee_log("%s: NULL vtable slots: %s", what, line);
  else
    ee_log("%s: no NULL vtable slots in first %u", what, slots);
}

/* writable_vt copies only `slots` entries into the replacement vtable. Anything
 * the interface defines BEYOND that is left uninitialised, so a call to a later
 * method jumps into garbage -- typically address 0. IDirect3DDevice7 has ~49
 * methods and IDirectDrawSurface7 ~50, so these counts must be generous, not
 * "enough for the slots we hook". Getting this wrong is what produced
 * "Unhandled page fault on read access to 00000000 at address 00000000"
 * immediately after CreateDevice. */
static void **writable_vt(void *obj, unsigned slots) {
  void **old;
  void **copy;
  if (!obj)
    return NULL;
  old = *(void ***)obj;
  copy = (void **)VirtualAlloc(NULL, slots * sizeof(void *), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!copy)
    return NULL;
  memcpy(copy, old, slots * sizeof(void *));
  *(void ***)obj = copy;
  return copy;
}

static HRESULT STDMETHODCALLTYPE hook_GetDisplayMode7(IDirectDraw7 *this, DDSURFACEDESC2 *desc) {
  HRESULT hr = orig_GetDisplayMode7 ? orig_GetDisplayMode7(this, desc) : DD_OK;
  /* This used to rewrite every answer to 800x600x32 unconditionally, because
   * dgVoodoo could not change the mode. On the D7VK stack that lie is suspect:
   * the game takes an exclusive cooperative level, asks what mode it is in, and
   * then aborts via the CRT (raise(22)) before ever calling SetDisplayMode.
   * Pass the truth through; EE_DDRAW_REAL_GETMODE=1 passes the truth through (set for the d7vk stack). */
  {
    static int fake = -1;
    if (fake < 0) {
      char buf[8];
      fake = (GetEnvironmentVariableA("EE_DDRAW_REAL_GETMODE", buf, sizeof buf) > 0 && buf[0] == '1') ? 0 : 1;
    }
    if (!fake) {
      if (desc && g_mode_w && g_mode_h) {
        ee_log("GetDisplayMode %lux%lu bpp=%lu -> reporting the mode the game set: %lux%lux%lu",
               (unsigned long)desc->dwWidth, (unsigned long)desc->dwHeight,
               (unsigned long)desc->ddpfPixelFormat.dwRGBBitCount, (unsigned long)g_mode_w,
               (unsigned long)g_mode_h, (unsigned long)g_mode_bpp);
        fill_dd_mode(desc, g_mode_w, g_mode_h, g_mode_bpp);
        return hr;
      }
      if (desc)
        ee_log("GetDisplayMode %lux%lu bpp=%lu (passed through)", (unsigned long)desc->dwWidth,
               (unsigned long)desc->dwHeight, (unsigned long)desc->ddpfPixelFormat.dwRGBBitCount);
      return hr;
    }
  }
  if (desc) {
    ee_log("GetDisplayMode orig %lux%lu bpp=%lu -> 800x600x32", (unsigned long)desc->dwWidth,
           (unsigned long)desc->dwHeight, (unsigned long)desc->ddpfPixelFormat.dwRGBBitCount);
    desc->dwFlags |= DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
    desc->dwWidth = 800;
    desc->dwHeight = 600;
    desc->dwRefreshRate = 60;
    desc->ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
    desc->ddpfPixelFormat.dwFlags = DDPF_RGB;
    desc->ddpfPixelFormat.dwRGBBitCount = 32;
    desc->ddpfPixelFormat.dwRBitMask = 0x00FF0000;
    desc->ddpfPixelFormat.dwGBitMask = 0x0000FF00;
    desc->ddpfPixelFormat.dwBBitMask = 0x000000FF;
  }
  (void)hr;
  return DD_OK;
}

/* This used to swallow SetDisplayMode and return DD_OK, because dgVoodoo could
 * not honour a real mode change under Wine. On the D7VK stack that stub is
 * actively harmful: the game goes fullscreen-exclusive and then asks for
 * 800x600, and if the wrapper never hears about it the primary surface is
 * created against the wrong mode. Forward it, and only fall back to pretending
 * if the wrapper genuinely refuses. EE_DDRAW_FAKE_SETMODE=1 restores the stub. */
static HRESULT STDMETHODCALLTYPE hook_SetDisplayMode7(IDirectDraw7 *this, DWORD w, DWORD h, DWORD bpp, DWORD refresh,
                                                      DWORD flags) {
  HRESULT hr;
  static int fake = -1;
  if (fake < 0) {
    char buf[8];
    fake = (GetEnvironmentVariableA("EE_DDRAW_REAL_SETMODE", buf, sizeof buf) > 0 && buf[0] == '1') ? 0 : 1;
  }
  if (fake) {
    ee_log("SetDisplayMode %lux%lux%lu refresh=%lu -> faked OK", (unsigned long)w, (unsigned long)h,
           (unsigned long)bpp, (unsigned long)refresh);
    /* Remember what was asked for: GetDisplayMode then reports it, and in full
     * screen the window watchdog sizes and centres the window to it. */
    g_mode_w = w;
    g_mode_h = h;
    g_mode_bpp = bpp;
    g_mode_frame0 = g_n_frames;
    return DD_OK;
  }
  ee_log("SetDisplayMode %lux%lux%lu refresh=%lu flags=0x%lx", (unsigned long)w, (unsigned long)h, (unsigned long)bpp,
         (unsigned long)refresh, (unsigned long)flags);
  pages_release("SetDisplayMode");
  hr = orig_SetDisplayMode7 ? orig_SetDisplayMode7(this, w, h, bpp, refresh, flags) : DD_OK;
  ee_log("SetDisplayMode hr=0x%08lx", (unsigned long)hr);
  /* Under Wine's emulated display modes (EmulateModeset) the game enumerates
   * 60 Hz modes and asks for exactly that refresh rate, which the switch then
   * rejects (0x80004001) -- and faking success below leaves the display at its
   * full size with the game drawing its 640x480 / 1024x768 screens into the
   * top-left corner.  0 means "whatever the mode's default is". */
  if (FAILED(hr) && refresh && orig_SetDisplayMode7) {
    hr = orig_SetDisplayMode7(this, w, h, bpp, 0, flags);
    ee_log("SetDisplayMode retry with refresh=0 hr=0x%08lx", (unsigned long)hr);
  }
  if (FAILED(hr)) {
    /* D7VK returns E_NOTIMPL (0x80004001) -- it has no real mode switch. Report
     * success, and remember what was asked for so GetDisplayMode agrees. Telling
     * the game the mode changed and then reporting the old desktop mode leaves
     * it with an incoherent view of the display it is rendering into. */
    ee_log("SetDisplayMode failed; reporting success so the game keeps going");
    hr = DD_OK;
  }
  if (SUCCEEDED(hr)) {
    g_mode_w = w;
    g_mode_h = h;
    g_mode_bpp = bpp;
    g_mode_frame0 = g_n_frames;
    drop_topmost_excl("mode switch", FALSE);
  }
  return hr;
}

#ifndef DDSCL_FULLSCREEN
#define DDSCL_FULLSCREEN 0x00000001
#endif
#ifndef DDSCL_EXCLUSIVE
#define DDSCL_EXCLUSIVE 0x00000010
#endif
#ifndef DDSCL_NORMAL
#define DDSCL_NORMAL 0x00000008
#endif
#ifndef DDSCL_MULTITHREADED
#define DDSCL_MULTITHREADED 0x00000400
#endif

/* ---- window state -------------------------------------------------------
 * 16 Sep: the first Flip finally showed its return value and it is
 * 0x887601c2 = DDERR_SURFACELOST, immediately followed by a COLORFILL whose
 * destination rect is -32000,-32000,-31360,-31520.  -32000,-32000 is Windows'
 * canonical iconic-window position, so at that moment the game's window is
 * MINIMIZED: exclusive-fullscreen DirectDraw loses its surfaces whenever the
 * owning window is deactivated, and the game then tears the device down and
 * re-enumerates from scratch -- which is the "full screen then blank" the
 * owner sees.  Log the window's actual state so this stops being inference. */
static HWND g_coop_hwnd;

static void log_window_state(const char *why) {
  HWND h = g_coop_hwnd;
  RECT rc;
  HWND fg;
  if (!h)
    return;
  memset(&rc, 0, sizeof rc);
  GetWindowRect(h, &rc);
  fg = GetForegroundWindow();
  ee_log("window[%s] hwnd=%p rect=%ld,%ld,%ld,%ld iconic=%d visible=%d enabled=%d fg=%p(%s) active=%p", why,
         (void *)h, (long)rc.left, (long)rc.top, (long)rc.right, (long)rc.bottom, (int)IsIconic(h),
         (int)IsWindowVisible(h), (int)IsWindowEnabled(h), (void *)fg, fg == h ? "ours" : "OTHER",
         (void *)GetActiveWindow());
  if (fg && fg != h) {
    char cls[128] = "", title[128] = "";
    DWORD pid = 0, tid;
    RECT fr;
    memset(&fr, 0, sizeof fr);
    GetClassNameA(fg, cls, sizeof cls);
    GetWindowTextA(fg, title, sizeof title);
    GetWindowRect(fg, &fr);
    tid = GetWindowThreadProcessId(fg, &pid);
    ee_log("    foreground thief %p class='%s' title='%s' rect=%ld,%ld,%ld,%ld pid=%lu tid=%lu", (void *)fg, cls,
           title, (long)fr.left, (long)fr.top, (long)fr.right, (long)fr.bottom, (unsigned long)pid,
           (unsigned long)tid);
  }
}

/* ---- keep the device window on screen -------------------------------------
 * Wine's macOS driver minimizes a fullscreen window when the application is
 * deactivated; DXVK's D3D9 swapchain then reports D3DERR_DEVICELOST for an
 * iconic window, D7VK turns that into DDERR_SURFACELOST, and Empire Earth
 * responds by tearing the device down and re-enumerating -- forever.  Nothing
 * below us can break that cycle, so a small watchdog thread un-minimizes the
 * device window and puts it back in the foreground.  EE_DDRAW_KEEP_FOREGROUND=0
 * disables it. */
/* Set as soon as a frame really renders.  Before that, an iconic window is the
 * bug (Wine minimized it behind our back and the device is about to be lost);
 * after that, an iconic window is the PLAYER minimizing the game, and fighting
 * them is how you trap someone behind a full-screen window they cannot escape. */
static volatile LONG g_rendering;
/* The window that actually took DDSCL_EXCLUSIVE -- as opposed to g_coop_hwnd,
 * which also collects the 16x16 probe windows the plugin scan creates. */
static HWND g_excl_hwnd;
/* Latched by the panic hotkey; from then on the watchdog never touches the
 * window again. */
static volatile LONG g_handsoff;

static void veto_focus_loss_minimize(void);

/* ---- no always-on-top ------------------------------------------------------
 * Wine's ddraw makes an exclusive full-screen window HWND_TOPMOST, and Wine 11's
 * Mac driver keeps every WS_EX_TOPMOST window at kCGDockWindowLevel + 1 whether
 * or not Wine is the active app (adjustWindowLevels' minFloatingLevel), which
 * overrides WindowsFloatWhenInactive.  So after Cmd-Tab the game stayed on top
 * of every other app as a full-screen black window.  Without the style the
 * driver lifts a full-screen window above the menu bar and Dock only while Wine
 * is active (NSStatusWindowLevel + 1) and drops it to the normal level when the
 * player switches away.  EE_DDRAW_KEEP_TOPMOST=1 keeps Wine's behaviour. */
static void drop_topmost_excl(const char *why, BOOL async) {
  static int want = -1;
  static int logged;
  static DWORD last_async;
  HWND h = g_excl_hwnd;
  if (want < 0) {
    char b[8];
    want = !(GetEnvironmentVariableA("EE_DDRAW_KEEP_TOPMOST", b, sizeof b) > 0 && b[0] == '1');
  }
  if (!want || !h || !IsWindow(h) || !(GetWindowLongA(h, GWL_EXSTYLE) & WS_EX_TOPMOST))
    return;
  /* SWP_ASYNCWINDOWPOS from the watchdog: SetWindowPos on another thread's
   * window otherwise waits for that thread to pump messages.  The request only
   * runs when the game's thread next pumps, which can be many seconds into a
   * match load, so do not stack up one per second. */
  if (async) {
    if (last_async && GetTickCount() - last_async < 5000)
      return;
    last_async = GetTickCount();
  }
  SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | (async ? SWP_ASYNCWINDOWPOS : 0));
  if (logged++ < 10)
    ee_log("hwnd=%p: dropped WS_EX_TOPMOST (%s) -- it kept the game above every app after Cmd-Tab", (void *)h,
           why);
}

static DWORD WINAPI keep_foreground(void *arg) {
  int restores = 0;
  int ticks = 0;
  int moves = 0;
  int stretched = 0;
  (void)arg;
  for (;;) {
    HWND h = g_coop_hwnd;
    /* Ctrl+Alt+Q -- let me out: release any cursor clip, stop restoring the
     * window, and minimize so macOS can show something else.
     * Ctrl+Alt+X -- quit the game outright.
     * Both act on the first poll that sees the chord.  A 300 ms hold was tried
     * on 22 Sep 2026 and swallowed three real quick-tap quits in a row. */
    {
      SHORT kc = GetAsyncKeyState(VK_CONTROL), ka = GetAsyncKeyState(VK_MENU);
      SHORT kq = GetAsyncKeyState('Q'), kx = GetAsyncKeyState('X');
      char key = ((kc & 0x8000) && (ka & 0x8000)) ? ((kx & 0x8000) ? 'X' : (kq & 0x8000) ? 'Q' : 0) : 0;
      if (key == 'Q') {
        if (!g_handsoff) {
          g_handsoff = 1;
          ee_log("PANIC Ctrl+Alt+Q: releasing the cursor and letting go of the window");
        }
        ClipCursor(NULL);
        while (ShowCursor(TRUE) < 0) {
        }
        if (h && IsWindow(h))
          ShowWindow(h, SW_MINIMIZE);
      }
      if (key == 'X') {
        ee_log("PANIC Ctrl+Alt+X: quitting (ctrl=%04x alt=%04x x=%04x)", (unsigned short)kc, (unsigned short)ka,
               (unsigned short)kx);
        ClipCursor(NULL);
        if (h && IsWindow(h))
          PostMessageA(h, WM_CLOSE, 0, 0);
        Sleep(1500);
        ExitProcess(0);
      }
    }
    /* A heartbeat every 2 s: it timestamps the window's state independently of
     * whatever the game's own thread is doing, so a stall that leaves no
     * DirectDraw trace still gets a timeline. */
    if ((ticks % 40) == 0 && ticks / 40 < 60)
      log_window_state("tick");
    /* Cheap liveness: if these numbers keep moving the game is loading, not
     * stalled.  Every 2 s for the whole session -- about 200 KB an hour -- so a
     * slowdown an hour into a match has the same timeline as one at launch. */
    if ((ticks % 40) == 20) {
      static LONG last_s, last_l, last_f, last_pn, last_pu, last_sn, last_vd;
      LONG cs = g_n_surfaces, cl = g_n_locks, cf = g_n_frames;
      /* The patched wow64cpu.dll (patches/wine/patch-wow64cpu.py) counts every
       * 32<->64-bit crossing Rosetta landed in the wrong mode and had to redo,
       * at .data RVA 0x2ff0 of the DLL Wine maps at 0x7bf20000.  Nonzero numbers
       * climbing during a hang would mean a thunk is spinning. */
      DWORD redo[3] = {0, 0, 0};
      SIZE_T got = 0;
      ReadProcessMemory(GetCurrentProcess(), (void *)0x7bf22ff0, redo, sizeof(redo), &got);
      LONG pn = g_pages_n, pu = g_pages_us;
      LONG dc_us = InterlockedExchange(&g_dc_us, 0), dc_n = InterlockedExchange(&g_dc_n, 0);
      LONG rdc_us = InterlockedExchange(&g_rdc_us, 0), rdc_n = InterlockedExchange(&g_rdc_n, 0);
      LONG flip_us = InterlockedExchange(&g_flip_us, 0), flip_n = InterlockedExchange(&g_flip_n, 0);
      ee_log("progress: surfaces=%ld(+%ld) locks=%ld(+%ld) frames=%ld(+%ld) blts=%ld redo=%lu/%lu/%lu "
             "pages=+%ld (%ld us each) softblt=+%ld fallback=%ld vbdraws=+%ld maxverts=%ld overruns=%ld bltfail=%ld "
             "bltfix=%ld dc=%ldus rdc=%ldus flip=%ldus ft=%ld/%ld/%ld/%ld/%ld/%ld/%ld/%ld/%ld max=%ldms",
             (long)cs, (long)(cs - last_s), (long)cl, (long)(cl - last_l), (long)cf, (long)(cf - last_f),
             (long)g_n_blts, got == sizeof(redo) ? redo[0] : 0UL, got == sizeof(redo) ? redo[1] : 0UL,
             got == sizeof(redo) ? redo[2] : 0UL, (long)(pn - last_pn),
             pn > last_pn ? (long)((pu - last_pu) / (pn - last_pn)) : 0L, (long)(g_soft_n - last_sn),
             (long)g_soft_fallback, (long)(g_vb_draws - last_vd), (long)g_vb_maxverts, (long)g_vb_overruns,
             (long)g_n_bltfail, (long)g_n_bltfix, dc_n ? (long)(dc_us / dc_n) : 0L, rdc_n ? (long)(rdc_us / rdc_n) : 0L,
             flip_n ? (long)(flip_us / flip_n) : 0L, (long)InterlockedExchange(&g_ft[0], 0),
             (long)InterlockedExchange(&g_ft[1], 0), (long)InterlockedExchange(&g_ft[2], 0),
             (long)InterlockedExchange(&g_ft[3], 0), (long)InterlockedExchange(&g_ft[4], 0),
             (long)InterlockedExchange(&g_ft[5], 0), (long)InterlockedExchange(&g_ft[6], 0),
             (long)InterlockedExchange(&g_ft[7], 0), (long)InterlockedExchange(&g_ft[8], 0),
             (long)InterlockedExchange(&g_ft_max, 0) / 1000);
      last_vd = g_vb_draws;
      last_sn = g_soft_n;
      last_pn = pn;
      last_pu = pu;
      last_s = cs;
      last_l = cl;
      last_f = cf;
    }
    if ((ticks % 20) == 0) {
      veto_focus_loss_minimize();
      if (!g_handsoff)
        drop_topmost_excl("watchdog", TRUE);
    }
    ticks++;
    /* Inside the virtual desktop the game's window sits at y=33, so its bottom
     * 33 rows fall off the desktop and the last main-menu entry is clipped in
     * half.  Three hard-won constraints on fixing that:
     *   - only ever touch the window that took the EXCLUSIVE cooperative level.
     *     g_coop_hwnd also holds the 16x16 probe windows from the plugin scan,
     *     and nudging those cost 8 launches out of 8.
     *   - only once the game is already rendering.  During device setup this
     *     blocks the game's own thread and it never reaches a frame.
     *   - SWP_ASYNCWINDOWPOS, because SetWindowPos on another thread's window
     *     otherwise waits for that thread to pump messages. */
    /* EE_DDRAW_STRETCH=1: once the game is rendering, grow its window to fill
     * the whole virtual desktop.  The game keeps rendering at its own mode --
     * the menu is fixed at 1024x768 no matter what the registry says -- and
     * DXVK scales that backbuffer up to the swapchain, so the picture gets
     * bigger without the game knowing anything changed.  Off by default: a
     * WM_SIZE can make a D3D7 title rebuild its device. */
    if (g_rendering && g_excl_hwnd && !g_handsoff && !stretched && IsWindow(g_excl_hwnd) &&
        !IsIconic(g_excl_hwnd)) {
      static int want = -1;
      if (want < 0) {
        char b[8];
        want = (GetEnvironmentVariableA("EE_DDRAW_STRETCH", b, sizeof b) > 0 && b[0] == '1') ? 1 : 0;
      }
      if (want) {
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        RECT rc;
        if (sw > 0 && sh > 0 && GetWindowRect(g_excl_hwnd, &rc) &&
            (rc.right - rc.left != sw || rc.bottom - rc.top != sh)) {
          ee_log("window watchdog: stretching %ldx%ld -> %dx%d to fill the desktop",
                 (long)(rc.right - rc.left), (long)(rc.bottom - rc.top), sw, sh);
          SetWindowPos(g_excl_hwnd, NULL, 0, 0, sw, sh,
                       SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        }
      }
      stretched = 1;
    }
    if (g_rendering && g_excl_hwnd && !g_handsoff && moves < 3 && IsWindow(g_excl_hwnd) &&
        !IsIconic(g_excl_hwnd)) {
      RECT rc;
      if (GetWindowRect(g_excl_hwnd, &rc) && (rc.left != 0 || rc.top != 0)) {
        ee_log("window watchdog: moving %ld,%ld -> 0,0 (%ldx%ld) (#%d)", (long)rc.left, (long)rc.top,
               (long)(rc.right - rc.left), (long)(rc.bottom - rc.top), moves + 1);
        SetWindowPos(g_excl_hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        moves++;
      } else {
        moves = 3; /* already at the origin -- stop looking */
      }
    }
    if (g_excl_hwnd)
      h = g_excl_hwnd;
    if (h && IsWindow(h) && IsIconic(h) && !g_rendering && !g_handsoff) {
      if (restores < 40)
        ee_log("window watchdog: hwnd=%p is iconic -- restoring (#%d)", (void *)h, restores + 1);
      ShowWindow(h, SW_RESTORE);
      ShowWindow(h, SW_SHOW);
      SetForegroundWindow(h);
      SetActiveWindow(h);
      BringWindowToTop(h);
      restores++;
    }
    Sleep(50);
  }
}

static void start_window_watchdog(void) {
  static int started;
  char buf[8];
  if (started)
    return;
  if (GetEnvironmentVariableA("EE_DDRAW_KEEP_FOREGROUND", buf, sizeof buf) > 0 && buf[0] == '0')
    return;
  started = 1;
  CloseHandle(CreateThread(NULL, 0, keep_foreground, NULL, 0, NULL));
  ee_log("window watchdog started");
}

static void bypass_wined3d_focus_hook(void);

/* ---- game window-message trace --------------------------------------------
 * Twice on 22 Sep 2026 the game's own thread sat looping in Sleep() while D7VK
 * created the TnL device with the window covering the whole screen: a start-up
 * that never finished, and a match start that spent 94 s inside CreateDevice.
 * Log the focus, activation and size messages the game window receives, and how
 * long the game's window procedure spends on each, so a handler that parks the
 * thread (an old game's "pause while inactive" loop) shows up with a duration.
 * EE_DDRAW_WNDTRACE=0 turns the logging off.  The same subclass defers
 * WM_ACTIVATEAPP (see trace_wndproc); EE_DDRAW_SYNC_ACTIVATEAPP=1 stops that. */
static WNDPROC g_game_wndproc;
static HWND g_traced_hwnd;
static BOOL g_traced_unicode;
static int g_wndtrace_log = 1, g_activateapp_sync;
static BOOL g_deactivate_pending; /* a deferred WM_ACTIVATEAPP(FALSE) is in the queue */
#define EE_WM_ACTIVATEAPP_LATER (WM_APP + 0x3e1)

static const char *traced_msg(UINT m) {
  switch (m) {
  case WM_ACTIVATEAPP: return "WM_ACTIVATEAPP";
  case WM_ACTIVATE: return "WM_ACTIVATE";
  case WM_NCACTIVATE: return "WM_NCACTIVATE";
  case WM_SETFOCUS: return "WM_SETFOCUS";
  case WM_KILLFOCUS: return "WM_KILLFOCUS";
  case WM_SIZE: return "WM_SIZE";
  case WM_SHOWWINDOW: return "WM_SHOWWINDOW";
  case WM_DISPLAYCHANGE: return "WM_DISPLAYCHANGE";
  case WM_SYSCOMMAND: return "WM_SYSCOMMAND";
  case WM_CLOSE: return "WM_CLOSE";
  case WM_QUERYENDSESSION: return "WM_QUERYENDSESSION";
  case WM_ENDSESSION: return "WM_ENDSESSION";
  case WM_CANCELMODE: return "WM_CANCELMODE";
  case WM_WINDOWPOSCHANGED: return "WM_WINDOWPOSCHANGED";
  default: return NULL;
  }
}

static LRESULT CALLBACK trace_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
  static unsigned n_posc, n_other;
  const char *name;
  DWORD t0, dt;
  LRESULT r;
  int loud;
  /* The game's WM_ACTIVATEAPP(FALSE) handler (Empire Earth.exe 0x662656) posts
   * to its cursor thread and spins until that thread answers, and the cursor
   * thread first waits for the game's UI lock.  The game polls DirectInput
   * while holding that lock (0x661889), and Wine's DirectInput pumps sent
   * messages inside GetDeviceState -- so a macOS app switch landing there ran
   * the handler nested inside the lock: deadlocked, frozen, and it could not be
   * quit (23 Sep 2026).  Re-post the deactivation instead, so the game handles
   * it from its own message loop, where it never holds the lock (that pump
   * dispatches only sent messages, never posted ones).  Activation stays
   * synchronous -- its handler waits for nothing, and the game's "paused while
   * inactive" loops wait on it -- and it cancels a deactivation still in the
   * queue, so switching away and straight back never leaves the game paused. */
  if (m == WM_ACTIVATEAPP && !g_activateapp_sync) {
    if (!w && (g_traced_unicode ? PostMessageW(h, EE_WM_ACTIVATEAPP_LATER, w, l)
                                : PostMessageA(h, EE_WM_ACTIVATEAPP_LATER, w, l))) {
      g_deactivate_pending = TRUE;
      if (g_wndtrace_log)
        ee_log("wndproc WM_ACTIVATEAPP w=0x0 l=0x%lx -- deferred to the message loop", (unsigned long)l);
      return 0;
    }
    if (w && g_deactivate_pending) {
      g_deactivate_pending = FALSE;
      if (g_wndtrace_log)
        ee_log("wndproc WM_ACTIVATEAPP: active again before the deferred deactivation ran; dropping it");
    }
  }
  if (m == EE_WM_ACTIVATEAPP_LATER) {
    if (!g_deactivate_pending)
      return 0;
    g_deactivate_pending = FALSE;
    m = WM_ACTIVATEAPP;
  }
  name = g_wndtrace_log ? traced_msg(m) : NULL;
  if (!name)
    return g_traced_unicode ? CallWindowProcW(g_game_wndproc, h, m, w, l)
                            : CallWindowProcA(g_game_wndproc, h, m, w, l);
  /* Quitting from the Mac side (Dock > Quit, Cmd+Q) arrives as WM_QUERYENDSESSION
   * and WM_ENDSESSION, after which winemac.drv TerminateProcess()es the game --
   * no DLL detach, no other trace.  Unlogged, that exit looked exactly like a
   * crash (23 Sep 2026), so these and WM_CLOSE are always logged. */
  loud = hot_ok(m == WM_WINDOWPOSCHANGED ? &n_posc : &n_other) || m == WM_ACTIVATEAPP || m == WM_ACTIVATE ||
         m == WM_CLOSE || m == WM_QUERYENDSESSION || m == WM_ENDSESSION;
  if (loud) {
    if (m == WM_WINDOWPOSCHANGED && l) {
      const WINDOWPOS *wp = (const WINDOWPOS *)l;
      ee_log("wndproc %s %d,%d %dx%d flags=0x%x", name, wp->x, wp->y, wp->cx, wp->cy, wp->flags);
    } else {
      ee_log("wndproc %s w=0x%lx l=0x%lx", name, (unsigned long)w, (unsigned long)l);
    }
  }
  t0 = GetTickCount();
  r = g_traced_unicode ? CallWindowProcW(g_game_wndproc, h, m, w, l) : CallWindowProcA(g_game_wndproc, h, m, w, l);
  dt = GetTickCount() - t0;
  if (dt >= 100)
    ee_log("wndproc %s w=0x%lx took %lu ms", name, (unsigned long)w, (unsigned long)dt);
  return r;
}

static void trace_game_wndproc(HWND hwnd) {
  static int want = -1;
  LONG_PTR cur;
  if (want < 0) {
    char b[8];
    g_wndtrace_log = !(GetEnvironmentVariableA("EE_DDRAW_WNDTRACE", b, sizeof b) > 0 && b[0] == '0');
    g_activateapp_sync = GetEnvironmentVariableA("EE_DDRAW_SYNC_ACTIVATEAPP", b, sizeof b) > 0 && b[0] == '1';
    want = g_wndtrace_log || !g_activateapp_sync;
  }
  if (!want || !hwnd || (g_traced_hwnd && IsWindow(g_traced_hwnd)))
    return;
  g_traced_unicode = IsWindowUnicode(hwnd);
  cur = g_traced_unicode ? GetWindowLongPtrW(hwnd, GWLP_WNDPROC) : GetWindowLongPtrA(hwnd, GWLP_WNDPROC);
  if (!cur || cur == (LONG_PTR)trace_wndproc)
    return;
  g_game_wndproc = (WNDPROC)cur;
  if (g_traced_unicode)
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)trace_wndproc);
  else
    SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)trace_wndproc);
  g_traced_hwnd = hwnd;
  ee_log("tracing focus/size messages to hwnd=%p (%s window proc %p)", (void *)hwnd,
         g_traced_unicode ? "unicode" : "ansi", (void *)cur);
}

static HRESULT STDMETHODCALLTYPE hook_SetCoop7(IDirectDraw7 *this, HWND hwnd, DWORD flags) {
  HRESULT hr;
  ee_log("SetCooperativeLevel hwnd=%p flags=0x%lx", (void *)hwnd, (unsigned long)flags);
  pages_release("SetCooperativeLevel");
  if (flags & DDSCL_EXCLUSIVE)
    bypass_wined3d_focus_hook();
  if (hwnd)
    g_coop_hwnd = hwnd;
  /* EE_DDRAW_WINDOWED=1: the game asks for DDSCL_FULLSCREEN|DDSCL_EXCLUSIVE and
   * then stops dead -- no SetDisplayMode, no primary surface, no error. There is
   * no real exclusive-fullscreen to hand it on macOS, so offer a windowed
   * cooperative level instead and let D7VK present into the 800x600 window the
   * game has already created. */
  if ((flags & (DDSCL_FULLSCREEN | DDSCL_EXCLUSIVE)) == (DDSCL_FULLSCREEN | DDSCL_EXCLUSIVE)) {
    char buf[8];
    if (GetEnvironmentVariableA("EE_DDRAW_WINDOWED", buf, sizeof buf) > 0 && buf[0] == '1') {
      DWORD keep = flags & DDSCL_MULTITHREADED;
      flags = DDSCL_NORMAL | keep;
      ee_log("SetCooperativeLevel downgraded exclusive-fullscreen -> 0x%lx", (unsigned long)flags);
    }
  }
  hr = orig_SetCoop7 ? orig_SetCoop7(this, hwnd, flags) : DD_OK;
  ee_log("SetCooperativeLevel hr=0x%08lx", (unsigned long)hr);
  log_window_state("after-setcoop");
  if (SUCCEEDED(hr) && (flags & DDSCL_EXCLUSIVE)) {
    g_excl_hwnd = hwnd;
    trace_game_wndproc(hwnd);
    drop_topmost_excl("exclusive mode", FALSE);
    start_window_watchdog();
  }
  return hr;
}

static HRESULT STDMETHODCALLTYPE hook_CreateSurface7(IDirectDraw7 *this, DDSURFACEDESC2 *desc,
                                                     IDirectDrawSurface7 **surf, IUnknown *outer) {
  HRESULT hr;
  static unsigned n;
  static unsigned n_fail; /* failures get their own budget */
  /* The primary is the one that matters; a loading screen creates thousands of
   * 256x256 texture surfaces.  Always log a primary (DDSCAPS_PRIMARYSURFACE),
   * throttle the rest. */
  int loud = (desc && (desc->ddsCaps.dwCaps & (DDSCAPS_PRIMARYSURFACE | DDSCAPS_ZBUFFER))) || hot_ok(&n);
  if (loud) {
    if (desc)
      ee_log("CreateSurface %lux%lu flags=0x%lx caps=0x%lx (dd=%p)", (unsigned long)desc->dwWidth,
             (unsigned long)desc->dwHeight, (unsigned long)desc->dwFlags, (unsigned long)desc->ddsCaps.dwCaps,
             (void *)this);
    else
      ee_log("CreateSurface desc=NULL");
  }
  if (desc && (desc->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE)) {
    pages_release("new primary");
    g_flips_since_chain = 0;
  }
  hr = orig_CreateSurface7 ? orig_CreateSurface7(this, desc, surf, outer) : DDERR_GENERIC;
  if (SUCCEEDED(hr))
    g_n_surfaces++;
  if (loud || (FAILED(hr) && hot_ok(&n_fail)))
    ee_log("CreateSurface hr=0x%08lx", (unsigned long)hr);
  {
    static int n;
    if (n++ < 6)
      log_window_state("after-createsurface");
  }
  if (SUCCEEDED(hr) && surf && *surf)
    wrap_surf7(*surf);
  return hr;
}

static HRESULT STDMETHODCALLTYPE hook_GetCaps7(IDirectDraw7 *this, DDCAPS *hel, DDCAPS *hw) {
  HRESULT hr = orig_GetCaps7 ? orig_GetCaps7(this, hel, hw) : DD_OK;
  ee_log("GetCaps hr=0x%08lx", (unsigned long)hr);
  /* Log only, and only what dwSize guarantees is present. Mutating this struct
   * (or reading past it) crashed the game inside DirectDrawEnumerateExA. */
  if (SUCCEEDED(hr) && hw && hw->dwSize >= sizeof(DDCAPS))
    ee_log("  HW caps=0x%lx surfcaps=0x%lx zdepths=0x%lx vidmem total=%lu free=%lu", (unsigned long)hw->dwCaps,
           (unsigned long)hw->ddsCaps.dwCaps, (unsigned long)hw->dwZBufferBitDepths,
           (unsigned long)hw->dwVidMemTotal, (unsigned long)hw->dwVidMemFree);
  return hr;
}

typedef struct {
  LPDDENUMMODESCALLBACK2 cb;
  LPVOID ctx;
  int saw_800_32;
  int saw_800_16;
  int total;
} enum_mode_ctx;

/* Through D7VK -> ddraw_.dll -> Wine's builtin ddraw, the real mode list is the
 * Mac desktop's own resolutions at 8bpp (2294x1490, 3024x1964, ...) and contains
 * no 800x600. Empire Earth is a 2001 Direct3D 7 title: it wants 16/32bpp modes
 * at classic sizes, and it stops dead after going fullscreen-exclusive when the
 * list it got is unusable. Hide the modes it cannot want. */
/* Filtering this list was tried on 15 Sep and REGRESSED the game: dropping the
 * 8bpp / oversized Mac desktop modes left Empire Earth with only the two
 * injected 800x600 entries and it died inside DirectDrawEnumerateExA at
 * 7BF2123D. It evidently needs the full list it was given. Keep the hook so the
 * modes stay visible in the log, but pass everything through. */
static int mode_is_sane(const DDSURFACEDESC2 *sd) {
  (void)sd;
  return 1;
}

static HRESULT CALLBACK tramp_enum_modes(DDSURFACEDESC2 *sd, void *ctx) {
  enum_mode_ctx *e = ctx;
  if (sd) {
    if (!mode_is_sane(sd)) {
      ee_log("  mode %lux%lu bpp=%lu (dropped)", (unsigned long)sd->dwWidth, (unsigned long)sd->dwHeight,
             (unsigned long)sd->ddpfPixelFormat.dwRGBBitCount);
      return DDENUMRET_OK;
    }
    ee_log("  mode %lux%lu bpp=%lu", (unsigned long)sd->dwWidth, (unsigned long)sd->dwHeight,
           (unsigned long)sd->ddpfPixelFormat.dwRGBBitCount);
    e->total++;
    /* Empire Earth.cpp:2506 asserts "theMode < theModeCount" after searching the
     * enumerated list for (width, height, BIT DEPTH). Tracking 800x600 without
     * the depth was wrong: through ddraw_.dll the real list is the Mac desktop's
     * modes at 8bpp, so an 800x600x8 entry would suppress the injection and the
     * game would never find the 800x600x32 it asks for. */
    if (sd->dwWidth == 800 && sd->dwHeight == 600) {
      if (sd->ddpfPixelFormat.dwRGBBitCount == 32)
        e->saw_800_32 = 1;
      else if (sd->ddpfPixelFormat.dwRGBBitCount == 16)
        e->saw_800_16 = 1;
    }
  }
  return e->cb ? e->cb(sd, e->ctx) : DDENUMRET_OK;
}

static void fill_dd_mode(DDSURFACEDESC2 *sd, DWORD w, DWORD h, DWORD bpp);
static void fill_800(DDSURFACEDESC2 *sd, DWORD bpp) { fill_dd_mode(sd, 800, 600, bpp); }

static void fill_dd_mode(DDSURFACEDESC2 *sd, DWORD w, DWORD h, DWORD bpp) {
  memset(sd, 0, sizeof *sd);
  sd->dwSize = sizeof *sd;
  sd->dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
  sd->dwWidth = w;
  sd->dwHeight = h;
  sd->dwRefreshRate = 60;
  sd->ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
  sd->ddpfPixelFormat.dwFlags = DDPF_RGB;
  sd->ddpfPixelFormat.dwRGBBitCount = bpp;
  if (bpp == 16) {
    sd->ddpfPixelFormat.dwRBitMask = 0xF800;
    sd->ddpfPixelFormat.dwGBitMask = 0x07E0;
    sd->ddpfPixelFormat.dwBBitMask = 0x001F;
  } else {
    sd->ddpfPixelFormat.dwRBitMask = 0x00FF0000;
    sd->ddpfPixelFormat.dwGBitMask = 0x0000FF00;
    sd->ddpfPixelFormat.dwBBitMask = 0x000000FF;
  }
}

static HRESULT STDMETHODCALLTYPE hook_EnumDisplayModes7(IDirectDraw7 *this, DWORD flags, DDSURFACEDESC2 *filter,
                                                        void *ctx, LPDDENUMMODESCALLBACK2 cb) {
  enum_mode_ctx e;
  HRESULT hr;
  DDSURFACEDESC2 extra;
  e.cb = cb;
  e.ctx = ctx;
  e.saw_800_32 = 0;
  e.saw_800_16 = 0;
  e.total = 0;
  ee_log("EnumDisplayModes flags=0x%lx", (unsigned long)flags);
  hr = orig_EnumDisplayModes7 ? orig_EnumDisplayModes7(this, flags, filter, &e, tramp_enum_modes) : DD_OK;
  /* Inject ONLY what is missing, and only once. Injecting the full classic set
   * on every call crashed the game at 7BF2123D: EnumDisplayModes runs inside the
   * adapter-enumeration callback and Empire Earth's resolution list is bounded,
   * so the duplicates overflowed it. */
  /* Injecting per-bit-depth (i.e. adding 800x600x32 even when an 800x600 entry
   * at another depth already existed) was tried on 16 Sep and destabilised the
   * run: the game hung at 108% CPU inside DirectDrawEnumerateExA for 150s+.
   * Keep the original all-or-nothing condition; the per-depth flags below are
   * diagnostics only, and they are what the next fix needs to reason about. */
  /* Empire Earth.exe+0x137518 calls the mode-search with (640, 480, bpp):
   *     mov $0x280,%edi ; push $0x1e0 ; push %edi ; call <search>
   * and Empire Earth.cpp:2506 asserts "theMode < theModeCount" when that search
   * falls off the end. Through ddraw_.dll the 66 real modes are the Mac
   * desktop's own resolutions -- there is no 640x480 among them and no 800x600
   * either, so the very first probe fails and the game aborts.
   * Inject both classic sizes at both depths. Keep this to ONE guarded block:
   * injecting unconditionally on every EnumDisplayModes call overflows the
   * game's bounded list and crashes it at 7BF2123D. */
  /* Empire Earth.exe searches this list from two different places:
   *   +0x137518 with a hardcoded 640x480, and
   *   +0x535497 with (width, height) from its caller plus the bit depth held in
   *   the global at 0x009193F4.
   * Both call Empire Earth.cpp:2506's "theMode < theModeCount" assert when the
   * search falls off the end. Cover the classic sizes at both depths. Keep it to
   * ONE guarded block -- injecting unconditionally on every call overflows the
   * game's bounded list and crashes it at 7BF2123D. */
  if (cb && !e.saw_800_32 && !e.saw_800_16) {
    static const DWORD sizes[][2] = {{640, 480}, {800, 600}, {1024, 768}};
    static const DWORD depths[] = {32, 16};
    unsigned si, di;
    /* Empire Earth.exe has a fixed base of 0x400000, so this global is readable
     * directly; it is the bit depth the second search path asks for. */
    const DWORD *want_bpp = (const DWORD *)0x009193F4;
    if (!IsBadReadPtr(want_bpp, sizeof *want_bpp))
      ee_log("EnumDisplayModes: game's desired bit depth global = %lu", (unsigned long)*want_bpp);
    for (si = 0; si < 3; si++)
      for (di = 0; di < 2; di++) {
        fill_dd_mode(&extra, sizes[si][0], sizes[si][1], depths[di]);
        ee_log("EnumDisplayModes inject %lux%lux%lu", (unsigned long)sizes[si][0], (unsigned long)sizes[si][1],
               (unsigned long)depths[di]);
        cb(&extra, ctx);
      }
  }
  ee_log("EnumDisplayModes hr=0x%08lx real=%d saw800x32=%d saw800x16=%d", (unsigned long)hr, e.total, e.saw_800_32,
         e.saw_800_16);
  return hr;
}

static HRESULT STDMETHODCALLTYPE hook_StartModeTest7(IDirectDraw7 *this, LPSIZE modes, DWORD n, DWORD flags) {
  (void)this;
  (void)modes;
  ee_log("StartModeTest n=%lu flags=0x%lx (stub pass)", (unsigned long)n, (unsigned long)flags);
  g_mode_test_left = n ? (int)n : 1;
  return DD_OK;
}

static HRESULT STDMETHODCALLTYPE hook_EvaluateMode7(IDirectDraw7 *this, DWORD flags, DWORD *secs) {
  (void)this;
  (void)flags;
  if (secs)
    *secs = 0;
  if (g_mode_test_left > 0) {
    g_mode_test_left--;
    ee_log("EvaluateMode -> DD_OK (%d left)", g_mode_test_left);
    return DD_OK;
  }
  ee_log("EvaluateMode -> TESTFINISHED");
  return DDERR_TESTFINISHED;
}

static HRESULT STDMETHODCALLTYPE hook_WaitVBlank7(IDirectDraw7 *this, DWORD flags, HANDLE ev) {
  static int n;
  (void)this;
  (void)flags;
  (void)ev;
  if (n < 8) {
    ee_log("WaitForVerticalBlank -> OK");
    n++;
  }
  return DD_OK;
}

static HRESULT STDMETHODCALLTYPE hook_GetVBlankStatus7(IDirectDraw7 *this, BOOL *on) {
  (void)this;
  if (on)
    *on = TRUE;
  return DD_OK;
}

static DWORD g_scanline;

static HRESULT STDMETHODCALLTYPE hook_GetScanLine7(IDirectDraw7 *this, DWORD *line) {
  (void)this;
  g_scanline = (g_scanline + 3) % 600;
  if (line)
    *line = g_scanline;
  return DD_OK;
}

static HRESULT(STDMETHODCALLTYPE *orig_WaitVBlank7)(IDirectDraw7 *, DWORD, HANDLE);
static HRESULT(STDMETHODCALLTYPE *orig_GetVBlankStatus7)(IDirectDraw7 *, BOOL *);
static HRESULT(STDMETHODCALLTYPE *orig_GetScanLine7)(IDirectDraw7 *, DWORD *);
static HRESULT(STDMETHODCALLTYPE *orig_SurfLock)(IDirectDrawSurface7 *, LPRECT, DDSURFACEDESC2 *, DWORD, HANDLE);
static HRESULT(STDMETHODCALLTYPE *orig_SurfGetDC)(IDirectDrawSurface7 *, HDC *);
static HRESULT(STDMETHODCALLTYPE *orig_SurfBlt)(IDirectDrawSurface7 *, LPRECT, IDirectDrawSurface7 *, LPRECT, DWORD,
                                                DDBLTFX *);

/* The device is created against the BACK BUFFER, not the primary we wrapped
 * (surface=01C40430 vs primary 01C5A330). That surface comes from
 * GetAttachedSurface (slot 12) and was never wrapped or scanned -- and the game
 * jumps to address 0 right after CreateDevice. Wrap what it hands back. */
static HRESULT(STDMETHODCALLTYPE *orig_SurfGetAttached)(IDirectDrawSurface7 *, DDSCAPS2 *, IDirectDrawSurface7 **);

/* Only three surfaces are ever created (2x2 probe, 16x16 probe, 640x480
 * primary) and none of them is the 01C40430 the game hands to CreateDevice --
 * so it obtains that pointer through a call we do not hook. Surface slot 0,
 * QueryInterface, is the remaining candidate: D7VK can hand back a sibling
 * interface object whose vtable we never patched. Log every QI on a surface,
 * and wrap anything surface-shaped that comes back. */
static HRESULT(STDMETHODCALLTYPE *orig_SurfQI)(IDirectDrawSurface7 *, REFIID, void **);

static HRESULT STDMETHODCALLTYPE hook_SurfQI(IDirectDrawSurface7 *this, REFIID riid, void **out) {
  HRESULT hr = orig_SurfQI ? orig_SurfQI(this, riid, out) : E_NOINTERFACE;
  const char *what = "other";
  int is_surface = 0;
  if (riid) {
    if (IsEqualGUID(riid, &IID_IDirectDrawSurface7)) {
      what = "IID_IDirectDrawSurface7";
      is_surface = 1;
    } else if (IsEqualGUID(riid, &IID_IDirectDrawSurface4)) {
      what = "IID_IDirectDrawSurface4";
      is_surface = 1;
    } else if (IsEqualGUID(riid, &IID_IDirectDrawSurface3)) {
      what = "IID_IDirectDrawSurface3";
      is_surface = 1;
    } else if (IsEqualGUID(riid, &IID_IDirectDrawSurface)) {
      what = "IID_IDirectDrawSurface";
      is_surface = 1;
    }
  }
  ee_log("Surface::QueryInterface %s (%s) this=%p hr=0x%08lx -> %p", what, guid_str(riid), (void *)this,
         (unsigned long)hr, (void *)(out ? *out : NULL));
  if (SUCCEEDED(hr) && out && *out && is_surface && *out != (void *)this)
    wrap_surf7(*out);
  return hr;
}

static HRESULT STDMETHODCALLTYPE hook_SurfGetAttached(IDirectDrawSurface7 *this, DDSCAPS2 *caps,
                                                      IDirectDrawSurface7 **out) {
  HRESULT hr = orig_SurfGetAttached ? orig_SurfGetAttached(this, caps, out) : DDERR_GENERIC;
  ee_log("Surface::GetAttachedSurface caps=0x%lx hr=0x%08lx -> %p", caps ? (unsigned long)caps->dwCaps : 0ul,
         (unsigned long)hr, (void *)(out ? *out : NULL));
  if (SUCCEEDED(hr) && out && *out)
    wrap_surf7(*out);
  /* The game fetches its back buffer with DDSCAPS_FLIP; its many texture
   * queries use DDSCAPS_MIPMAP. */
  if (SUCCEEDED(hr) && out && *out && caps && (caps->dwCaps & (DDSCAPS_FLIP | DDSCAPS_BACKBUFFER)) &&
      !(caps->dwCaps & DDSCAPS_MIPMAP)) {
    g_chain_front = this;
    g_chain_back = *out;
  }
  return hr;
}

static HRESULT STDMETHODCALLTYPE hook_SurfLock(IDirectDrawSurface7 *this, LPRECT r, DDSURFACEDESC2 *sd, DWORD flags,
                                               HANDLE ev) {
  HRESULT hr;
  static unsigned n;
  int loud = hot_ok(&n);
  if (loud)
    ee_log("Surface::Lock flags=0x%lx", (unsigned long)flags);
  if (this == g_chain_back)
    g_locked_back = 1;
  {
    IDirectDrawSurface7 *pp = g_pages_primary;
    int guard = pp && this == pp;
    if (guard) {
      static unsigned n_warn;
      if (hot_ok(&n_warn))
        ee_log("pages: the game locked the primary (not redirected to the front page)");
      EnterCriticalSection(&g_pages_cs);
    }
    hr = orig_SurfLock ? orig_SurfLock(this, r, sd, flags, ev) : DDERR_GENERIC;
    if (guard)
      LeaveCriticalSection(&g_pages_cs);
  }
  g_n_locks++;
  if (loud)
    ee_log("Surface::Lock hr=0x%08lx", (unsigned long)hr);
  return hr;
}

static HRESULT STDMETHODCALLTYPE hook_SurfGetDC(IDirectDrawSurface7 *this, HDC *dc) {
  HRESULT hr;
  static unsigned n;
  static unsigned n_fail; /* failures get their own budget */
  int loud = hot_ok(&n);
  if (loud)
    ee_log("Surface::GetDC");
  if (this == g_chain_back || this == g_chain_front)
    InterlockedIncrement(&g_dcs);
  if (this == g_chain_front) {
    static unsigned n_warn;
    if (hot_ok(&n_warn))
      ee_log("pages: GetDC on the primary (not redirected to the front page)");
  }
  {
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    hr = orig_SurfGetDC ? orig_SurfGetDC(this, dc) : DDERR_GENERIC;
    if (this == g_chain_back) {
      InterlockedExchangeAdd(&g_dc_us, us_since(&t0));
      InterlockedIncrement(&g_dc_n);
    }
  }
  if (loud || (FAILED(hr) && hot_ok(&n_fail)))
    ee_log("Surface::GetDC hr=0x%08lx", (unsigned long)hr);
  return hr;
}

static HRESULT(STDMETHODCALLTYPE *orig_SurfReleaseDC)(IDirectDrawSurface7 *, HDC);
static HRESULT STDMETHODCALLTYPE hook_SurfReleaseDC(IDirectDrawSurface7 *this, HDC dc) {
  LARGE_INTEGER t0;
  HRESULT hr;
  QueryPerformanceCounter(&t0);
  hr = orig_SurfReleaseDC ? orig_SurfReleaseDC(this, dc) : DDERR_GENERIC;
  if (this == g_chain_back) {
    InterlockedExchangeAdd(&g_rdc_us, us_since(&t0));
    InterlockedIncrement(&g_rdc_n);
  }
  return hr;
}

/* The primary is FLIP|COMPLEX, so the game presents by calling Flip on it.
 * Everything renders (Clear, Blt, 82 Locks, 106 surfaces) yet the screen is
 * solid black at full-screen size -- so presentation is the missing link.
 * Slot 11 = Flip. */
static HRESULT(STDMETHODCALLTYPE *orig_SurfFlip)(IDirectDrawSurface7 *, IDirectDrawSurface7 *, DWORD);

/* ---- page flipping, emulated on the DirectDraw side -----------------------
 * On its 2D screens (the menus) Empire Earth relies on real two-page flipping:
 * after Flip the back buffer holds the page that was on screen and the primary
 * holds the page just shown.  Two threads depend on it.  The render loop saves
 * the spot under the cursor, draws the cursor into the back buffer, flips, and
 * wipes it off the new back page with the save from the frame before; a second
 * thread moves the cursor on the visible page between frames by drawing
 * straight onto the primary -- both through the same two 32x32 save surfaces.
 * D7VK presents the back buffer through D3D9 but never exchanges the DirectDraw
 * surfaces: the back buffer keeps the frame just shown, and the primary is a
 * shadow surface (DXVK-Sarek's Empire Earth profile sets forceLegacyPresent)
 * that nothing on screen comes from and that starts out black.  The saves then
 * pick up old cursors and black, and the menu fills with cursor trails and
 * black squares (22 Sep 2026).
 * So exchange the two pages' contents at each flip: spare <- back, flip (D7VK
 * presents the back buffer), back <- front, front <- spare, where "front" is
 * our own page standing in for D7VK's primary (see g_pages_front).  Match frames
 * (3D draws plus the game's per-frame text lock of the back buffer) repaint
 * the whole back buffer, so there only the primary is brought up to date
 * (front <- back after the flip) -- skipping them entirely left the front page
 * stale: the menus draw each new screen in a few frames with 3D draws, the new
 * screen never reached the other page, and the menu flickered with a mostly
 * black page and 32x32 squares around the cursor (23 Sep 2026).
 * The cursor thread's blits on the primary take the same lock, so they never
 * see a half-exchanged pair.
 * Following DXVK's own D3D9 page rotation instead (22 Sep, first attempt) fixed
 * only the back page and made the menu flicker.  EE_DDRAW_PAGES=0 turns this
 * off. */
static IDirectDrawSurface7 *g_pages_spare; /* owned */
/* Owned: the page on screen, standing in for D7VK's primary.  Every D3D draw
 * marks D7VK's primary "newer on the D3D9 side", so the next Lock or Blt of it
 * first downloads the D3D9 back buffer into it -- mid-frame, that wiped the
 * front page to black (23 Sep 2026).  The game's blits on the primary are
 * redirected here instead; D7VK never downloads into a plain offscreen surface. */
static IDirectDrawSurface7 *g_pages_front;
static DWORD g_pages_w, g_pages_h, g_pages_retry;

/* ---- software blits on the flip chain --------------------------------------
 * D7VK keeps DirectDraw's copy of every surface in Wine's ddraw, i.e. in
 * wined3d over Apple's OpenGL.  A Blt on the back buffer runs on the GPU
 * there and leaves the GL texture as the only current copy, so D7VK's next
 * Lock of the back buffer -- the game's text lock every frame and D7VK's own
 * upload at the flip -- reads the whole frame back with glGetTexImage: about
 * 30% of a match frame (ee-prof with csmt off, 22 Sep 2026).  Doing the
 * game's small blits on the flip chain (cursor save, draw and restore, and
 * the page exchange) on the CPU through D7VK's own Lock/Unlock keeps those
 * surfaces in system memory, where the Locks cost nothing.
 * Plain copies and source colour keys between two different surfaces of the
 * same 16/32-bit format, without stretching; anything else, or any failure,
 * goes to the real Blt.  EE_DDRAW_SOFTBLT=0 turns this off. */
static int softblt_on(void) {
  static int on = -1;
  if (on < 0) {
    char b[8];
    on = !(GetEnvironmentVariableA("EE_DDRAW_SOFTBLT", b, sizeof b) > 0 && b[0] == '0');
    ee_log("softblt: %s", on ? "blits on the flip chain run on the CPU" : "off (EE_DDRAW_SOFTBLT=0)");
  }
  return on;
}

static void soft_fallback(const char *why) {
  LONG n = InterlockedIncrement(&g_soft_fallback);
  if (n <= 10)
    ee_log("softblt: real Blt instead (%s)", why);
}

/* Returns DD_OK when done, S_FALSE to hand the blit to the real Blt. */
static HRESULT soft_blt(IDirectDrawSurface7 *dst, LPRECT dr, IDirectDrawSurface7 *src, LPRECT sr, DWORD flags) {
  const DWORD allowed = DDBLT_WAIT | DDBLT_DONOTWAIT | DDBLT_ASYNC | DDBLT_KEYSRC;
  DDSURFACEDESC2 dd, sd, ld, ls;
  RECT d, s;
  DDCOLORKEY ck;
  DWORD key = 0, mask, bpp, y;
  LONG w, h;
  int keyed = (flags & DDBLT_KEYSRC) != 0;
  if (!src || src == dst || !orig_SurfLock)
    return S_FALSE;
  if (flags & ~allowed) {
    soft_fallback("flags");
    return S_FALSE;
  }
  memset(&dd, 0, sizeof dd);
  dd.dwSize = sizeof dd;
  memset(&sd, 0, sizeof sd);
  sd.dwSize = sizeof sd;
  if (FAILED(dst->lpVtbl->GetSurfaceDesc(dst, &dd)) || FAILED(src->lpVtbl->GetSurfaceDesc(src, &sd)))
    return S_FALSE;
  if (dr)
    d = *dr;
  else
    SetRect(&d, 0, 0, (int)dd.dwWidth, (int)dd.dwHeight);
  if (sr)
    s = *sr;
  else
    SetRect(&s, 0, 0, (int)sd.dwWidth, (int)sd.dwHeight);
  w = d.right - d.left;
  h = d.bottom - d.top;
  if (w <= 0 || h <= 0 || w != s.right - s.left || h != s.bottom - s.top) {
    soft_fallback("stretch or empty");
    return S_FALSE;
  }
  if (d.left < 0 || d.top < 0 || d.right > (LONG)dd.dwWidth || d.bottom > (LONG)dd.dwHeight || s.left < 0 ||
      s.top < 0 || s.right > (LONG)sd.dwWidth || s.bottom > (LONG)sd.dwHeight) {
    soft_fallback("rectangle outside the surface");
    return S_FALSE;
  }
  if (keyed) {
    if (FAILED(src->lpVtbl->GetColorKey(src, DDCKEY_SRCBLT, &ck)) ||
        ck.dwColorSpaceLowValue != ck.dwColorSpaceHighValue) {
      soft_fallback("colour key");
      return S_FALSE;
    }
    key = ck.dwColorSpaceLowValue;
  }
  memset(&ls, 0, sizeof ls);
  ls.dwSize = sizeof ls;
  memset(&ld, 0, sizeof ld);
  ld.dwSize = sizeof ld;
  if (FAILED(orig_SurfLock(src, &s, &ls, DDLOCK_WAIT | DDLOCK_READONLY, NULL)))
    return S_FALSE;
  if (FAILED(orig_SurfLock(dst, &d, &ld, DDLOCK_WAIT | DDLOCK_WRITEONLY, NULL))) {
    src->lpVtbl->Unlock(src, &s);
    return S_FALSE;
  }
  bpp = ld.ddpfPixelFormat.dwRGBBitCount;
  if ((bpp != 16 && bpp != 32) || bpp != ls.ddpfPixelFormat.dwRGBBitCount ||
      ld.ddpfPixelFormat.dwRBitMask != ls.ddpfPixelFormat.dwRBitMask ||
      ld.ddpfPixelFormat.dwGBitMask != ls.ddpfPixelFormat.dwGBitMask ||
      ld.ddpfPixelFormat.dwBBitMask != ls.ddpfPixelFormat.dwBBitMask || !ld.lpSurface || !ls.lpSurface) {
    dst->lpVtbl->Unlock(dst, &d);
    src->lpVtbl->Unlock(src, &s);
    soft_fallback("pixel format");
    return S_FALSE;
  }
  mask = ld.ddpfPixelFormat.dwRBitMask | ld.ddpfPixelFormat.dwGBitMask | ld.ddpfPixelFormat.dwBBitMask;
  for (y = 0; y < (DWORD)h; y++) {
    const BYTE *srow = (const BYTE *)ls.lpSurface + (LONG_PTR)y * ls.lPitch;
    BYTE *drow = (BYTE *)ld.lpSurface + (LONG_PTR)y * ld.lPitch;
    LONG x;
    if (!keyed) {
      memcpy(drow, srow, (size_t)w * (bpp / 8));
    } else if (bpp == 32) {
      const DWORD *sp = (const DWORD *)srow;
      DWORD *dp = (DWORD *)drow, k = key & mask;
      for (x = 0; x < w; x++)
        if ((sp[x] & mask) != k)
          dp[x] = sp[x];
    } else {
      const WORD *sp = (const WORD *)srow;
      WORD *dp = (WORD *)drow, k = (WORD)(key & mask);
      for (x = 0; x < w; x++)
        if ((sp[x] & (WORD)mask) != k)
          dp[x] = sp[x];
    }
  }
  dst->lpVtbl->Unlock(dst, &d);
  src->lpVtbl->Unlock(src, &s);
  InterlockedIncrement(&g_soft_n);
  return DD_OK;
}

/* A whole-surface copy for the page exchange: on the CPU when possible. */
static HRESULT pages_copy(IDirectDrawSurface7 *dst, IDirectDrawSurface7 *src) {
  if (softblt_on() && soft_blt(dst, NULL, src, NULL, DDBLT_WAIT) == DD_OK)
    return DD_OK;
  return orig_SurfBlt(dst, NULL, src, NULL, DDBLT_WAIT, NULL);
}

static int pages_on(void) {
  static int on = -1;
  if (on < 0) {
    char b[8];
    on = !(GetEnvironmentVariableA("EE_DDRAW_PAGES", b, sizeof b) > 0 && b[0] == '0');
    ee_log("pages: %s", on ? "2D frames exchange back buffer and primary at each flip" : "off (EE_DDRAW_PAGES=0)");
  }
  return on;
}

/* Called with g_pages_cs held. */
static void pages_release_locked(const char *why) {
  if (g_pages_front) {
    g_pages_front->lpVtbl->Release(g_pages_front);
    g_pages_front = NULL;
  }
  if (g_pages_spare) {
    g_pages_spare->lpVtbl->Release(g_pages_spare);
    g_pages_spare = NULL;
    ee_log("pages: spare and front pages released (%s)", why);
  }
  g_pages_primary = NULL;
  g_pages_retry = 0;
}

/* Before a mode change or a new DirectDraw object: the spare holds a reference
 * to the DirectDraw object it was made from, which must not outlive the game's. */
static void pages_release(const char *why) {
  if (!pages_on())
    return;
  EnterCriticalSection(&g_pages_cs);
  pages_release_locked(why);
  LeaveCriticalSection(&g_pages_cs);
}

/* Called with g_pages_cs held.  Returns the back buffer of this primary's flip
 * chain (not AddRef'd -- the chain keeps it) once a spare page exists for it. */
static IDirectDrawSurface7 *pages_prepare(IDirectDrawSurface7 *primary) {
  DDSCAPS2 caps;
  DDSURFACEDESC2 bd, sd;
  IDirectDrawSurface7 *back = NULL;
  IDirectDraw7 *dd = NULL;
  HRESULT hr;
  memset(&caps, 0, sizeof caps);
  caps.dwCaps = DDSCAPS_BACKBUFFER;
  if (!orig_SurfGetAttached || FAILED(orig_SurfGetAttached(primary, &caps, &back)) || !back)
    return NULL;
  back->lpVtbl->Release(back);
  memset(&bd, 0, sizeof bd);
  bd.dwSize = sizeof bd;
  if (FAILED(back->lpVtbl->GetSurfaceDesc(back, &bd)))
    return NULL;
  if (g_pages_spare && (g_pages_primary != primary || g_pages_w != bd.dwWidth || g_pages_h != bd.dwHeight))
    pages_release_locked("new flip chain");
  if (g_pages_spare)
    return back;
  if (g_pages_retry && GetTickCount() - g_pages_retry < 2000)
    return NULL;
  g_pages_retry = GetTickCount() | 1;
  if (!orig_CreateSurface7 || FAILED(back->lpVtbl->GetDDInterface(back, (void **)&dd)) || !dd)
    return NULL;
  memset(&sd, 0, sizeof sd);
  sd.dwSize = sizeof sd;
  sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
  sd.dwWidth = bd.dwWidth;
  sd.dwHeight = bd.dwHeight;
  if (bd.ddpfPixelFormat.dwSize) {
    sd.dwFlags |= DDSD_PIXELFORMAT;
    sd.ddpfPixelFormat = bd.ddpfPixelFormat;
  }
  sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;
  hr = orig_CreateSurface7(dd, &sd, &g_pages_spare, NULL);
  if (SUCCEEDED(hr) && g_pages_spare) {
    hr = orig_CreateSurface7(dd, &sd, &g_pages_front, NULL);
    if (FAILED(hr) || !g_pages_front) {
      g_pages_front = NULL;
      g_pages_spare->lpVtbl->Release(g_pages_spare);
      g_pages_spare = NULL;
    }
  }
  dd->lpVtbl->Release(dd);
  if (FAILED(hr) || !g_pages_spare) {
    g_pages_spare = NULL;
    ee_log("pages: could not create %lux%lu spare/front pages (hr=0x%08lx)", (unsigned long)bd.dwWidth,
           (unsigned long)bd.dwHeight, (unsigned long)hr);
    return NULL;
  }
  g_pages_primary = primary;
  g_pages_w = bd.dwWidth;
  g_pages_h = bd.dwHeight;
  g_pages_retry = 0;
  ee_log("pages: %lux%lu spare page %p, front page %p for primary %p (back buffer %p)", (unsigned long)bd.dwWidth,
         (unsigned long)bd.dwHeight, (void *)g_pages_spare, (void *)g_pages_front, (void *)primary, (void *)back);
  return back;
}

/* Debug: EE_DDRAW_PAGEDUMP=1 writes the pages at the first flips of each new
 * flip chain to the game directory as pagedump-<flip>-<what>.bmp. */
static int pagedump_on(void) {
  static int on = -1;
  if (on < 0) {
    char b[8];
    on = (GetEnvironmentVariableA("EE_DDRAW_PAGEDUMP", b, sizeof b) > 0 && b[0] == '1');
  }
  return on;
}

static void dump_page(IDirectDrawSurface7 *surf, LONG flip, const char *what) {
  DDSURFACEDESC2 d;
  char name[64];
  FILE *f;
  DWORD y, w, h;
  if (!surf || !orig_SurfLock)
    return;
  memset(&d, 0, sizeof d);
  d.dwSize = sizeof d;
  if (FAILED(orig_SurfLock(surf, NULL, &d, DDLOCK_WAIT | DDLOCK_READONLY, NULL)))
    return;
  w = d.dwWidth;
  h = d.dwHeight;
  snprintf(name, sizeof name, "pagedump-%02ld-%s.bmp", (long)flip, what);
  if (d.ddpfPixelFormat.dwRGBBitCount == 32 && d.lpSurface && (f = fopen(name, "wb")) != NULL) {
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    memset(&fh, 0, sizeof fh);
    memset(&ih, 0, sizeof ih);
    fh.bfType = 0x4d42;
    fh.bfOffBits = sizeof fh + sizeof ih;
    fh.bfSize = fh.bfOffBits + w * h * 4;
    ih.biSize = sizeof ih;
    ih.biWidth = (LONG)w;
    ih.biHeight = -(LONG)h;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&ih, sizeof ih, 1, f);
    for (y = 0; y < h; y++)
      fwrite((const BYTE *)d.lpSurface + (LONG_PTR)y * d.lPitch, 4, w, f);
    fclose(f);
  }
  surf->lpVtbl->Unlock(surf, NULL);
}

static HRESULT STDMETHODCALLTYPE hook_SurfFlip(IDirectDrawSurface7 *this, IDirectDrawSurface7 *target, DWORD flags) {
  static int n;
  HRESULT hr;
  IDirectDrawSurface7 *back = NULL;
  LARGE_INTEGER t0, t1, t2, t3, freq;
  int paged = 0, drew_3d = InterlockedExchange(&g_drew_3d, 0), locked = InterlockedExchange(&g_locked_back, 0);
  LONG draws = InterlockedExchange(&g_draws, 0), dcs = InterlockedExchange(&g_dcs, 0);
  /* A match frame redraws the whole back buffer, so only the front page needs
   * the frame (one copy after the flip) instead of a full exchange (three).
   * GetDC for the HUD text brings the finished frame into the back buffer just
   * as a Lock does -- and in big matches (25 Sep 2026) the game only uses
   * GetDC, so every frame took the 740 us exchange.  EE_DDRAW_MATCH_GETDC=0
   * goes back to counting only locked frames. */
  static int getdc_counts = -1;
  int match_frame;
  if (getdc_counts < 0) {
    char b[8];
    getdc_counts = !(GetEnvironmentVariableA("EE_DDRAW_MATCH_GETDC", b, sizeof b) > 0 && b[0] == '0');
  }
  match_frame = drew_3d && (locked || (getdc_counts && dcs > 0));
  if (!target && InterlockedIncrement(&g_flips_since_chain) <= 30)
    ee_log("pages: flip %ld of this chain: %ld draw calls, %ld GetDC, back buffer %slocked", (long)g_flips_since_chain,
           (long)draws, (long)dcs, locked ? "" : "not ");
  {
    static unsigned h;
    if (hot_ok(&h))
      ee_log("Surface::Flip ENTER this=%p target=%p flags=0x%lx", (void *)this, (void *)target,
             (unsigned long)flags);
  }
  if (!target && orig_SurfBlt && pages_on()) {
    QueryPerformanceCounter(&t0);
    EnterCriticalSection(&g_pages_cs);
    back = pages_prepare(this);
    if (back && match_frame) {
      /* front page <- back, BEFORE the flip: right now the back buffer's
       * DirectDraw copy is current (GetDC or Lock just downloaded the frame),
       * while after the flip D7VK downloads it from the GPU all over again --
       * 700 us a frame (25 Sep 2026).  Same content: the frame about to show. */
      paged = 2;
      if (FAILED(pages_copy(g_pages_front, back))) {
        paged = 0; /* fall back to the full exchange */
        if (SUCCEEDED(pages_copy(g_pages_spare, back))) {
          paged = 1;
        } else {
          back = NULL;
          LeaveCriticalSection(&g_pages_cs);
        }
      }
    } else if (back && SUCCEEDED(pages_copy(g_pages_spare, back))) {
      paged = 1;
      if (pagedump_on() && g_flips_since_chain <= 6)
        dump_page(g_pages_spare, g_flips_since_chain, "a-presented");
    } else {
      back = NULL;
      LeaveCriticalSection(&g_pages_cs);
    }
    QueryPerformanceCounter(&t1);
  }
  {
    LARGE_INTEGER tf;
    QueryPerformanceCounter(&tf);
    hr = orig_SurfFlip ? orig_SurfFlip(this, target, flags) : DDERR_GENERIC;
    InterlockedExchangeAdd(&g_flip_us, us_since(&tf));
    InterlockedIncrement(&g_flip_n);
  }
  /* Backstop for a lost surface (the main fix is declining wined3d's focus
   * hook, see hook_acquire_focus_window).  Wine's ddraw keeps surfaces "lost"
   * until Restore/RestoreAllSurfaces, which Empire Earth never calls mid-match.
   * Its Restore only clears flags -- no texture contents are lost -- so once
   * the game is back in front, restore on its behalf and flip again.  At most
   * once every half second.  EE_DDRAW_AUTO_RESTORE=0 turns this off. */
  if (hr == DDERR_SURFACELOST && g_excl_hwnd && GetForegroundWindow() == g_excl_hwnd && !IsIconic(g_excl_hwnd)) {
    static int enabled = -1;
    static DWORD last;
    static LONG attempts;
    DWORD now = GetTickCount();
    if (enabled < 0) {
      char b[8];
      enabled = !(GetEnvironmentVariableA("EE_DDRAW_AUTO_RESTORE", b, sizeof b) > 0 && b[0] == '0');
    }
    if (enabled && now - last >= 500) {
      IDirectDraw7 *dd = NULL;
      HRESULT all = E_FAIL, own = E_FAIL, again;
      last = now;
      if (SUCCEEDED(this->lpVtbl->GetDDInterface(this, (void **)&dd)) && dd) {
        all = dd->lpVtbl->RestoreAllSurfaces(dd);
        dd->lpVtbl->Release(dd);
      }
      own = this->lpVtbl->Restore(this);
      again = orig_SurfFlip(this, target, flags);
      attempts++;
      if (attempts <= 10 || SUCCEEDED(again))
        ee_log("Surface::Flip lost -> RestoreAllSurfaces=0x%08lx Restore=0x%08lx, flip again=0x%08lx (#%ld)",
               (unsigned long)all, (unsigned long)own, (unsigned long)again, (long)attempts);
      if (SUCCEEDED(again))
        hr = again;
    }
  }
  if (paged) {
    if (SUCCEEDED(hr)) {
      static unsigned n_log;
      HRESULT h1, h2 = E_FAIL;
      QueryPerformanceCounter(&t2);
      if (paged == 1) {
        h1 = pages_copy(back, g_pages_front);
        if (SUCCEEDED(h1))
          h2 = pages_copy(g_pages_front, g_pages_spare);
      } else {
        h1 = h2 = S_OK; /* front page already updated before the flip */
      }
      QueryPerformanceCounter(&t3);
      QueryPerformanceFrequency(&freq);
      InterlockedIncrement(&g_pages_n);
      if (freq.QuadPart)
        InterlockedExchangeAdd(&g_pages_us, (LONG)(((t1.QuadPart - t0.QuadPart) + (t3.QuadPart - t2.QuadPart)) *
                                                   1000000 / freq.QuadPart));
      if (pagedump_on() && g_flips_since_chain <= 6) {
        dump_page(back, g_flips_since_chain, "b-back-after");
        dump_page(g_pages_front, g_flips_since_chain, "c-front-after");
      }
      if (hot_ok(&n_log) || FAILED(h1) || FAILED(h2))
        ee_log("pages: %s after flip (back<-front hr=0x%08lx, front<-%s hr=0x%08lx)",
               paged == 1 ? "exchanged" : "match frame, primary updated", (unsigned long)h1,
               paged == 1 ? "spare" : "back", (unsigned long)h2);
    }
    LeaveCriticalSection(&g_pages_cs);
  }
  if (n++ < 12 || FAILED(hr) || (n % 20000) == 0)
    ee_log("Surface::Flip this=%p target=%p flags=0x%lx hr=0x%08lx%s (#%d)", (void *)this, (void *)target,
           (unsigned long)flags, (unsigned long)hr, hr == DDERR_SURFACELOST ? " DDERR_SURFACELOST" : "", n);
  if (FAILED(hr) && n <= 20)
    log_window_state("flip-failed");
  return hr;
}

/* "0x12345678 640x480 caps=0x840 32bpp R00ff0000 G0000ff00 B000000ff A00000000" */
static void describe_surf(IDirectDrawSurface7 *s, char *buf, size_t n) {
  DDSURFACEDESC2 d;
  const DDPIXELFORMAT *pf = &d.ddpfPixelFormat;
  if (!s) {
    snprintf(buf, n, "none");
    return;
  }
  memset(&d, 0, sizeof d);
  d.dwSize = sizeof d;
  if (FAILED(s->lpVtbl->GetSurfaceDesc(s, &d))) {
    snprintf(buf, n, "%p (no desc)", (void *)s);
    return;
  }
  if (pf->dwFlags & DDPF_FOURCC)
    snprintf(buf, n, "%p %lux%lu caps=0x%lx fourcc=%.4s", (void *)s, (unsigned long)d.dwWidth,
             (unsigned long)d.dwHeight, (unsigned long)d.ddsCaps.dwCaps, (const char *)&pf->dwFourCC);
  else
    snprintf(buf, n, "%p %lux%lu caps=0x%lx %lubpp R%08lx G%08lx B%08lx A%08lx%s", (void *)s,
             (unsigned long)d.dwWidth, (unsigned long)d.dwHeight, (unsigned long)d.ddsCaps.dwCaps,
             (unsigned long)pf->dwRGBBitCount, (unsigned long)pf->dwRBitMask, (unsigned long)pf->dwGBitMask,
             (unsigned long)pf->dwBBitMask, (unsigned long)pf->dwRGBAlphaBitMask,
             (pf->dwFlags & DDPF_PALETTEINDEXED8) ? " pal8" : "");
}

static HRESULT STDMETHODCALLTYPE hook_SurfBlt(IDirectDrawSurface7 *this, LPRECT dst, IDirectDrawSurface7 *src, LPRECT sr,
                                              DWORD flags, DDBLTFX *fx) {
  HRESULT hr;
  IDirectDrawSurface7 *to = this, *from = src;
  {
    static int n;
    if (n++ < 200)
      ee_log("Surface::Blt ENTER dst=%p src=%p flags=0x%lx dstrect=%ld,%ld,%ld,%ld", (void *)this, (void *)src,
             (unsigned long)flags, dst ? (long)dst->left : -1L, dst ? (long)dst->top : -1L,
             dst ? (long)dst->right : -1L, dst ? (long)dst->bottom : -1L);
  }
  /* The game presents frame #1 fine (Clear + Flip both hr=0) and then hangs
   * forever inside a DDBLT_COLORFILL (flags 0x400, src=NULL) on the back buffer
   * -- the Blt never returns, the process sits at 0% CPU, and the screen stays
   * black. Skipping the fill is a blunt workaround, but it tells us whether
   * colour-fill is really the blocker and lets the frame loop continue.
   * EE_DDRAW_COLORFILL=1 restores the real call. */
#ifndef DDBLT_COLORFILL
#define DDBLT_COLORFILL 0x00000400
#endif
  if ((flags & DDBLT_COLORFILL) && !src) {
    static int skip = -1;
    if (skip < 0) {
      char buf[8];
      skip = (GetEnvironmentVariableA("EE_DDRAW_COLORFILL", buf, sizeof buf) > 0 && buf[0] == '1') ? 0 : 1;
    }
    if (skip) {
      static int n;
      if (n++ < 5) {
        ee_log("Surface::Blt COLORFILL on %p skipped (it never returns)", (void *)this);
        log_window_state("colorfill");
      }
      return DD_OK;
    }
  }
  {
    /* The cursor thread draws on the primary; never let it see the pages half
     * exchanged (see pages_prepare). */
    IDirectDrawSurface7 *pp = g_pages_primary;
    int guard = pp && (this == pp || src == pp);
    IDirectDrawSurface7 *cf = g_chain_front, *cb = g_chain_back;
    int chain = cb && (this == cb || src == cb || this == cf || src == cf);
    if (guard)
      EnterCriticalSection(&g_pages_cs);
    /* The primary's content lives in our front page (see g_pages_front). */
    if (guard && g_pages_front && g_pages_primary == pp) {
      if (to == pp)
        to = g_pages_front;
      if (from == pp)
        from = g_pages_front;
    }
    hr = S_FALSE;
    if (chain && softblt_on())
      hr = soft_blt(to, dst, from, sr, flags);
    if (hr == S_FALSE)
      hr = orig_SurfBlt ? orig_SurfBlt(to, dst, from, sr, flags, fx) : DDERR_GENERIC;
    /* D7VK has no Blt into a texture: in a match the game copies a 64x64
     * A4R4G4B4 system-memory image into a texture every frame (flags
     * DDBLT_DONOTWAIT) and D7VK answers E_NOTIMPL, ~90 times a second on
     * 25 Sep 2026, so that texture never changed.  A same-format copy is
     * exactly what soft_blt does, through D7VK's own Lock/Unlock. */
    if (hr == E_NOTIMPL && from && soft_blt(to, dst, from, sr, flags) == DD_OK) {
      if (InterlockedIncrement(&g_n_bltfix) == 1)
        ee_log("softblt: a Blt the real Blt refused (E_NOTIMPL, flags 0x%lx) is now copied on the CPU", (unsigned long)flags);
      hr = DD_OK;
    }
    if (guard)
      LeaveCriticalSection(&g_pages_cs);
  }
  g_n_blts++;
  if (FAILED(hr)) {
    /* The first 20 failures in full; after that only the heartbeat's bltfail=
     * count (a custom map on 25 Sep 2026 failed ~90 blits a second with
     * E_NOTIMPL, one log line each, and nothing said which blit). */
    LONG nf = InterlockedIncrement(&g_n_bltfail);
    if (nf <= 20) {
      char ds[160], ss[160];
      describe_surf(to, ds, sizeof ds);
      describe_surf(from, ss, sizeof ss);
      ee_log("Surface::Blt FAILED hr=0x%08lx (#%ld) flags=0x%lx fx=0x%lx dst[%s] %ld,%ld,%ld,%ld src[%s] %ld,%ld,%ld,%ld%s%s",
             (unsigned long)hr, (long)nf, (unsigned long)flags, fx ? (unsigned long)fx->dwDDFX : 0UL, ds,
             dst ? (long)dst->left : -1L, dst ? (long)dst->top : -1L, dst ? (long)dst->right : -1L,
             dst ? (long)dst->bottom : -1L, ss, sr ? (long)sr->left : -1L, sr ? (long)sr->top : -1L,
             sr ? (long)sr->right : -1L, sr ? (long)sr->bottom : -1L,
             (to == g_chain_back || from == g_chain_back) ? " [back buffer]" : "",
             (to == g_pages_front || from == g_pages_front) ? " [front page]" : "");
    }
  } else {
    static unsigned n;
    if (hot_ok(&n))
      ee_log("Surface::Blt hr=0x%08lx", (unsigned long)hr);
  }
  return hr;
}

/* ---- IDirect3D7: the path that was invisible until 15 Sep 2026 -------------
 * DX7HRDisplay.dll / DX7HRTnLDisplay.dll import only DirectDrawEnumerateA,
 * DirectDrawCreateEx and DirectDrawEnumerateExA, and nothing at all from
 * d3dim/D3DImm. So the IDirect3D7 interface can ONLY reach them through
 * IDirectDraw7::QueryInterface -- vtable slot 0, which wrap_ddraw7() never
 * hooked. Every EnumDevices/CreateDevice call was therefore unlogged.
 * IDirect3D7 vtable: 3 EnumDevices, 4 CreateDevice, 5 CreateVertexBuffer,
 * 6 EnumZBufferFormats.
 */
static HRESULT(STDMETHODCALLTYPE *orig_QI7)(IDirectDraw7 *, REFIID, void **);
static HRESULT(STDMETHODCALLTYPE *orig_D3DEnumDevices)(IDirect3D7 *, LPD3DENUMDEVICESCALLBACK7, void *);
static HRESULT(STDMETHODCALLTYPE *orig_D3DCreateDevice)(IDirect3D7 *, REFCLSID, IDirectDrawSurface7 *,
                                                        IDirect3DDevice7 **);
static HRESULT(STDMETHODCALLTYPE *orig_D3DEnumZBuffer)(IDirect3D7 *, REFCLSID, LPD3DENUMPIXELFORMATSCALLBACK, void *);

static const char *guid_str(const GUID *g) {
  static char buf[64];
  if (!g)
    return "(null)";
  wsprintfA(buf, "{%08lX-%04X-%04X-%02X%02X%02X%02X%02X%02X%02X%02X}", (unsigned long)g->Data1, g->Data2, g->Data3,
            g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
  return buf;
}

static const char *device_name(const GUID *g) {
  if (!g)
    return "(null)";
  if (IsEqualGUID(g, &IID_IDirect3DTnLHalDevice))
    return "IID_IDirect3DTnLHalDevice";
  if (IsEqualGUID(g, &IID_IDirect3DHALDevice))
    return "IID_IDirect3DHALDevice";
  if (IsEqualGUID(g, &IID_IDirect3DRGBDevice))
    return "IID_IDirect3DRGBDevice";
  if (IsEqualGUID(g, &IID_IDirect3DRefDevice))
    return "IID_IDirect3DRefDevice";
  return guid_str(g);
}

typedef struct {
  LPD3DENUMDEVICESCALLBACK7 cb;
  void *ctx;
} enum_dev_ctx;

static HRESULT CALLBACK tramp_enum_devices(LPSTR desc, LPSTR name, LPD3DDEVICEDESC7 dd, void *ctx) {
  enum_dev_ctx *e = ctx;
  if (dd)
    /* dwDeviceZBufferBitDepth is what a D3D7 title reads to decide whether a
     * Z-buffer is worth creating.  The game never calls EnumZBufferFormats and
     * never creates a DDSCAPS_ZBUFFER surface, so every Clear(D3DCLEAR_ZBUFFER)
     * comes back D3DERR_ZBUFFER_NOTPRESENT (0x88760816) -- this is where to
     * look for the reason. */
    ee_log("  device \"%s\" / \"%s\" guid=%s devcaps=0x%lx maxtex=%lu zdepths=0x%lx zcmp=0x%lx",
           name ? name : "?", desc ? desc : "?", device_name(&dd->deviceGUID), (unsigned long)dd->dwDevCaps,
           (unsigned long)dd->wMaxSimultaneousTextures, (unsigned long)dd->dwDeviceZBufferBitDepth,
           (unsigned long)dd->dpcTriCaps.dwZCmpCaps);
  else
    ee_log("  device \"%s\" (no desc)", name ? name : "?");
  return e->cb ? e->cb(desc, name, dd, e->ctx) : D3DENUMRET_OK;
}

static HRESULT STDMETHODCALLTYPE hook_D3DEnumDevices(IDirect3D7 *this, LPD3DENUMDEVICESCALLBACK7 cb, void *ctx) {
  enum_dev_ctx e;
  HRESULT hr;
  e.cb = cb;
  e.ctx = ctx;
  ee_log("IDirect3D7::EnumDevices");
  hr = orig_D3DEnumDevices ? orig_D3DEnumDevices(this, tramp_enum_devices, &e) : DDERR_GENERIC;
  ee_log("IDirect3D7::EnumDevices hr=0x%08lx", (unsigned long)hr);
  return hr;
}

static HRESULT STDMETHODCALLTYPE hook_D3DCreateDevice(IDirect3D7 *this, REFCLSID rclsid, IDirectDrawSurface7 *surf,
                                                      IDirect3DDevice7 **dev) {
  HRESULT hr;
  DWORD t0;
  ee_log("IDirect3D7::CreateDevice %s this=%p surface=%p", device_name(rclsid), (void *)this, (void *)surf);
  t0 = GetTickCount();
  hr = orig_D3DCreateDevice ? orig_D3DCreateDevice(this, rclsid, surf, dev) : DDERR_GENERIC;
  ee_log("IDirect3D7::CreateDevice hr=0x%08lx dev=%p (%lu ms)", (unsigned long)hr, (void *)(dev ? *dev : NULL),
         (unsigned long)(GetTickCount() - t0));
  /* Creating the device through Wine's ddraw sets the window topmost again. */
  if (SUCCEEDED(hr))
    drop_topmost_excl("device created", FALSE);
  if (SUCCEEDED(hr) && dev && *dev)
    wrap_d3ddev7(*dev);
  return hr;
}

static HRESULT STDMETHODCALLTYPE hook_D3DEnumZBuffer(IDirect3D7 *this, REFCLSID rclsid,
                                                     LPD3DENUMPIXELFORMATSCALLBACK cb, void *ctx) {
  HRESULT hr;
  ee_log("IDirect3D7::EnumZBufferFormats %s", device_name(rclsid));
  hr = orig_D3DEnumZBuffer ? orig_D3DEnumZBuffer(this, rclsid, cb, ctx) : DDERR_GENERIC;
  ee_log("IDirect3D7::EnumZBufferFormats hr=0x%08lx", (unsigned long)hr);
  return hr;
}

/* IDirect3DDevice7 is created inside the probe and again for the real device,
 * and the game calls into it from its WM_SIZE / WM_WINDOWPOSCHANGING handler --
 * which is exactly where it aborts. Nothing wrapped that interface until now.
 * Slots: 3 GetCaps, 5 BeginScene, 6 EndScene, 8 SetRenderTarget, 10 Clear.
 * Log-only: every hook forwards unconditionally. */
static HRESULT(STDMETHODCALLTYPE *orig_DevGetCaps)(IDirect3DDevice7 *, D3DDEVICEDESC7 *);
static HRESULT(STDMETHODCALLTYPE *orig_DevBeginScene)(IDirect3DDevice7 *);
static HRESULT(STDMETHODCALLTYPE *orig_DevEndScene)(IDirect3DDevice7 *);
static HRESULT(STDMETHODCALLTYPE *orig_DevSetRT)(IDirect3DDevice7 *, IDirectDrawSurface7 *, DWORD);
static HRESULT(STDMETHODCALLTYPE *orig_DevClear)(IDirect3DDevice7 *, DWORD, D3DRECT *, DWORD, D3DCOLOR, D3DVALUE,
                                                 DWORD);
static HRESULT(STDMETHODCALLTYPE *orig_DevGetRT)(IDirect3DDevice7 *, IDirectDrawSurface7 **);

static HRESULT STDMETHODCALLTYPE hook_DevGetCaps(IDirect3DDevice7 *this, D3DDEVICEDESC7 *d) {
  HRESULT hr = orig_DevGetCaps ? orig_DevGetCaps(this, d) : DDERR_GENERIC;
  ee_log("Device7::GetCaps hr=0x%08lx", (unsigned long)hr);
  if (SUCCEEDED(hr) && d)
    ee_log("  devcaps=0x%lx zdepths=0x%lx zcmp=0x%lx devrendbitdepth=0x%lx", (unsigned long)d->dwDevCaps,
           (unsigned long)d->dwDeviceZBufferBitDepth, (unsigned long)d->dpcTriCaps.dwZCmpCaps,
           (unsigned long)d->dwDeviceRenderBitDepth);
  return hr;
}

static HRESULT STDMETHODCALLTYPE hook_DevBeginScene(IDirect3DDevice7 *this) {
  HRESULT hr;
  static unsigned n;
  static unsigned n_fail; /* failures get their own budget */
  int loud = hot_ok(&n);
  if (loud)
    ee_log("Device7::BeginScene ENTER this=%p", (void *)this);
  {
    /* The render thread's x87 mode, whenever it changes: precision (bits 8-9)
     * and rounding (10-11) decide how the engine's x87 maths rounds, which
     * ee-version's SSE replacements reproduce per call. */
    static unsigned short last_cw = 0xffff;
    unsigned short cw;
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    if (cw != last_cw) {
      static const char *pc[4] = {"24-bit", "reserved", "53-bit", "64-bit"}, *rc[4] = {"nearest", "down", "up", "chop"};
      last_cw = cw;
      ee_log("x87 control word on the render thread: 0x%04x (%s precision, round %s)", cw, pc[(cw >> 8) & 3],
             rc[(cw >> 10) & 3]);
    }
  }
  hr = orig_DevBeginScene ? orig_DevBeginScene(this) : DDERR_GENERIC;
  g_n_frames++;
  {
    static LARGE_INTEGER prev;
    if (prev.QuadPart) {
      const LONG us = us_since(&prev);
      const int b = us < 8000 ? 0 : us < 11000 ? 1 : us < 14000 ? 2 : us < 17000 ? 3 : us < 20000 ? 4 : us < 25000 ? 5
                  : us < 33000 ? 6 : us < 50000 ? 7 : 8;
      InterlockedIncrement(&g_ft[b]);
      if (us > g_ft_max)
        g_ft_max = us;
    }
    QueryPerformanceCounter(&prev);
  }

  if (SUCCEEDED(hr) && !g_rendering) {
    g_rendering = 1;
    ee_log("first BeginScene succeeded -- window watchdog now hands the window back to the player");
  }
  if (loud || (FAILED(hr) && hot_ok(&n_fail)))
    ee_log("Device7::BeginScene hr=0x%08lx", (unsigned long)hr);
  return hr;
}

static HRESULT STDMETHODCALLTYPE hook_DevEndScene(IDirect3DDevice7 *this) {
  HRESULT hr;
  static unsigned n;
  static unsigned n_fail; /* failures get their own budget */
  int loud = hot_ok(&n);
  if (loud)
    ee_log("Device7::EndScene ENTER this=%p", (void *)this);
  hr = orig_DevEndScene ? orig_DevEndScene(this) : DDERR_GENERIC;
  if (loud || (FAILED(hr) && hot_ok(&n_fail)))
    ee_log("Device7::EndScene hr=0x%08lx", (unsigned long)hr);
  return hr;
}

static HRESULT STDMETHODCALLTYPE hook_DevSetRT(IDirect3DDevice7 *this, IDirectDrawSurface7 *rt, DWORD flags) {
  HRESULT hr = orig_DevSetRT ? orig_DevSetRT(this, rt, flags) : DDERR_GENERIC;
  ee_log("Device7::SetRenderTarget surface=%p hr=0x%08lx", (void *)rt, (unsigned long)hr);
  return hr;
}

static HRESULT STDMETHODCALLTYPE hook_DevClear(IDirect3DDevice7 *this, DWORD count, D3DRECT *rects, DWORD flags,
                                               D3DCOLOR color, D3DVALUE z, DWORD stencil) {
  HRESULT hr;
  /* Log BEFORE the inner call: an ENTER with no matching exit line is direct
   * proof that this is the call that never returns.  Every exit-only log in
   * this file conflates "never called" with "called and hung". */
  static unsigned n;
  static unsigned n_fail; /* failures get their own budget */
  int loud = hot_ok(&n);
  if (loud)
    ee_log("Device7::Clear ENTER flags=0x%lx count=%lu", (unsigned long)flags, (unsigned long)count);
  hr = orig_DevClear ? orig_DevClear(this, count, rects, flags, color, z, stencil) : DDERR_GENERIC;
  if (loud || (FAILED(hr) && hot_ok(&n_fail)))
    ee_log("Device7::Clear flags=0x%lx hr=0x%08lx", (unsigned long)flags, (unsigned long)hr);
  return hr;
}

/* After SetRenderTarget the game makes no further *hooked* D3D call and spins.
 * The obvious next moves for a D3D7 title are EnumTextureFormats (pick surface
 * formats), GetRenderTarget, SetViewport, SetTransform and SetRenderState --
 * none of which were hooked, so any of them could be happening invisibly, or
 * failing. Slots: 4 EnumTextureFormats, 9 GetRenderTarget, 11 SetTransform,
 * 13 SetViewport, 20 SetRenderState. All log-only. */
static HRESULT(STDMETHODCALLTYPE *orig_DevEnumTex)(IDirect3DDevice7 *, LPD3DENUMPIXELFORMATSCALLBACK, void *);
static HRESULT(STDMETHODCALLTYPE *orig_DevSetTransform)(IDirect3DDevice7 *, D3DTRANSFORMSTATETYPE, D3DMATRIX *);
static HRESULT(STDMETHODCALLTYPE *orig_DevSetViewport)(IDirect3DDevice7 *, D3DVIEWPORT7 *);
static HRESULT(STDMETHODCALLTYPE *orig_DevSetRS)(IDirect3DDevice7 *, D3DRENDERSTATETYPE, DWORD);

static int g_texfmt_count;

static HRESULT CALLBACK tramp_enum_texfmt(DDPIXELFORMAT *pf, void *ctx) {
  void **u = ctx;
  LPD3DENUMPIXELFORMATSCALLBACK cb = (LPD3DENUMPIXELFORMATSCALLBACK)u[0];
  g_texfmt_count++;
  if (pf && g_texfmt_count <= 12)
    ee_log("  texfmt flags=0x%lx bpp=%lu fourcc=0x%lx", (unsigned long)pf->dwFlags,
           (unsigned long)pf->dwRGBBitCount, (unsigned long)pf->dwFourCC);
  return cb ? cb(pf, u[1]) : D3DENUMRET_OK;
}

static HRESULT STDMETHODCALLTYPE hook_DevEnumTex(IDirect3DDevice7 *this, LPD3DENUMPIXELFORMATSCALLBACK cb, void *ctx) {
  void *u[2];
  HRESULT hr;
  u[0] = (void *)cb;
  u[1] = ctx;
  g_texfmt_count = 0;
  ee_log("Device7::EnumTextureFormats");
  hr = orig_DevEnumTex ? orig_DevEnumTex(this, tramp_enum_texfmt, u) : DDERR_GENERIC;
  ee_log("Device7::EnumTextureFormats hr=0x%08lx formats=%d", (unsigned long)hr, g_texfmt_count);
  return hr;
}

static HRESULT STDMETHODCALLTYPE hook_DevGetRT(IDirect3DDevice7 *this, IDirectDrawSurface7 **rt) {
  HRESULT hr = orig_DevGetRT ? orig_DevGetRT(this, rt) : DDERR_GENERIC;
  ee_log("Device7::GetRenderTarget hr=0x%08lx -> %p", (unsigned long)hr, (void *)(rt ? *rt : NULL));
  return hr;
}

static HRESULT STDMETHODCALLTYPE hook_DevSetTransform(IDirect3DDevice7 *this, D3DTRANSFORMSTATETYPE st,
                                                      D3DMATRIX *m) {
  HRESULT hr = orig_DevSetTransform ? orig_DevSetTransform(this, st, m) : DDERR_GENERIC;
  static int n;
  if (n++ < 8)
    ee_log("Device7::SetTransform state=%d hr=0x%08lx", (int)st, (unsigned long)hr);
  return hr;
}

static HRESULT STDMETHODCALLTYPE hook_DevSetViewport(IDirect3DDevice7 *this, D3DVIEWPORT7 *vp) {
  HRESULT hr = orig_DevSetViewport ? orig_DevSetViewport(this, vp) : DDERR_GENERIC;
  static unsigned n;
  if (!hot_ok(&n))
    return hr;
  if (vp)
    ee_log("Device7::SetViewport %lux%lu at %lu,%lu hr=0x%08lx", (unsigned long)vp->dwWidth,
           (unsigned long)vp->dwHeight, (unsigned long)vp->dwX, (unsigned long)vp->dwY, (unsigned long)hr);
  else
    ee_log("Device7::SetViewport hr=0x%08lx", (unsigned long)hr);
  return hr;
}

static HRESULT STDMETHODCALLTYPE hook_DevSetRS(IDirect3DDevice7 *this, D3DRENDERSTATETYPE st, DWORD val) {
  HRESULT hr = orig_DevSetRS ? orig_DevSetRS(this, st, val) : DDERR_GENERIC;
  /* A flat cap of 20 was exhausted by the menu's opening state block, so the
   * in-game set was never visible.  Log each render state once instead. */
  static unsigned char seen[256];
  if ((unsigned)st < 256 && !seen[(unsigned)st]) {
    seen[(unsigned)st] = 1;
    ee_log("Device7::SetRenderState %d = 0x%lx hr=0x%08lx", (int)st, (unsigned long)val, (unsigned long)hr);
  } else if (FAILED(hr)) {
    ee_log("Device7::SetRenderState %d = 0x%lx FAILED hr=0x%08lx", (int)st, (unsigned long)val,
           (unsigned long)hr);
  }
  return hr;
}

/* ---- vertex ranges of the game's draws (diagnostics) --------------------------
 * In dense crowds (150+ units in one spot) some units blink in and out
 * (23 Sep 2026).  The renderer streams unit vertices through small dynamic
 * vertex buffers -- 2442 vertices, DISCARD on wrap, NOOVERWRITE appends
 * (DX7HRTnLDisplay.dll 0x1000a5a2), which D7VK and DXVK handle correctly.  But
 * if a draw reaches past the end of its vertices -- a buffer's size, or the
 * vertex count of a user-pointer draw, which is all DXVK uploads -- a Windows
 * driver reads neighbouring memory while Vulkan/Metal's bounds checks return
 * zeros, and exactly those units would vanish.  Count such draws. */
#define VB_TABLE 128
static struct {
  void *vb;
  DWORD verts;
} g_vbs[VB_TABLE];

static DWORD vb_size(void *vb) {
  LONG i, n = g_nvbs;
  if (n > VB_TABLE)
    n = VB_TABLE;
  for (i = n - 1; i >= 0; i--)
    if (g_vbs[i].vb == vb)
      return g_vbs[i].verts;
  return 0;
}

/* size: vertices available to the draw (0 = unknown); start/num: the range
 * the game declared; idx: its indices, relative to start. */
static void vb_check(const char *what, DWORD size, DWORD start, DWORD num, const WORD *idx, DWORD nidx) {
  DWORD top = start + num, i, maxi = 0;
  InterlockedIncrement(&g_vb_draws);
  if ((LONG)num > g_vb_maxverts)
    g_vb_maxverts = (LONG)num;
  if (idx && nidx) {
    for (i = 0; i < nidx; i++)
      if (idx[i] > maxi)
        maxi = idx[i];
    if (start + maxi + 1 > top)
      top = start + maxi + 1;
  }
  if (size && top > size) {
    LONG n = InterlockedIncrement(&g_vb_overruns);
    if (n <= 20)
      ee_log("vb: %s reaches vertex %lu but only %lu exist (start %lu, count %lu, max index %lu, %lu indices)", what,
             (unsigned long)top, (unsigned long)size, (unsigned long)start, (unsigned long)num,
             (unsigned long)maxi, (unsigned long)nidx);
  }
}

/* ---- Direct3D draws, for the page exchange ----------------------------------
 * A frame with any of these is redrawn in 3D (a match) and is not exchanged at
 * its flip; the menus only BeginScene/EndScene around 2D blits.  Slots 25, 26,
 * 29, 30, 31 and 32 of IDirect3DDevice7. */
static HRESULT(STDMETHODCALLTYPE *orig_DevDrawPrim)(IDirect3DDevice7 *, D3DPRIMITIVETYPE, DWORD, void *, DWORD, DWORD);
static HRESULT(STDMETHODCALLTYPE *orig_DevDrawIdxPrim)(IDirect3DDevice7 *, D3DPRIMITIVETYPE, DWORD, void *, DWORD,
                                                       WORD *, DWORD, DWORD);
static HRESULT(STDMETHODCALLTYPE *orig_DevDrawPrimStrided)(IDirect3DDevice7 *, D3DPRIMITIVETYPE, DWORD,
                                                           D3DDRAWPRIMITIVESTRIDEDDATA *, DWORD, DWORD);
static HRESULT(STDMETHODCALLTYPE *orig_DevDrawIdxPrimStrided)(IDirect3DDevice7 *, D3DPRIMITIVETYPE, DWORD,
                                                              D3DDRAWPRIMITIVESTRIDEDDATA *, DWORD, WORD *, DWORD,
                                                              DWORD);
static HRESULT(STDMETHODCALLTYPE *orig_DevDrawPrimVB)(IDirect3DDevice7 *, D3DPRIMITIVETYPE, IDirect3DVertexBuffer7 *,
                                                      DWORD, DWORD, DWORD);
static HRESULT(STDMETHODCALLTYPE *orig_DevDrawIdxPrimVB)(IDirect3DDevice7 *, D3DPRIMITIVETYPE,
                                                         IDirect3DVertexBuffer7 *, DWORD, DWORD, WORD *, DWORD, DWORD);

static HRESULT STDMETHODCALLTYPE hook_DevDrawPrim(IDirect3DDevice7 *this, D3DPRIMITIVETYPE t, DWORD fvf, void *v,
                                                  DWORD n, DWORD f) {
  g_drew_3d = 1;
  InterlockedIncrement(&g_draws);
  return orig_DevDrawPrim(this, t, fvf, v, n, f);
}

static HRESULT STDMETHODCALLTYPE hook_DevDrawIdxPrim(IDirect3DDevice7 *this, D3DPRIMITIVETYPE t, DWORD fvf, void *v,
                                                     DWORD n, WORD *idx, DWORD ni, DWORD f) {
  g_drew_3d = 1;
  InterlockedIncrement(&g_draws);
  vb_check("DrawIndexedPrimitive", n, 0, n, idx, ni);
  return orig_DevDrawIdxPrim(this, t, fvf, v, n, idx, ni, f);
}

static HRESULT STDMETHODCALLTYPE hook_DevDrawPrimStrided(IDirect3DDevice7 *this, D3DPRIMITIVETYPE t, DWORD fvf,
                                                         D3DDRAWPRIMITIVESTRIDEDDATA *d, DWORD n, DWORD f) {
  g_drew_3d = 1;
  InterlockedIncrement(&g_draws);
  return orig_DevDrawPrimStrided(this, t, fvf, d, n, f);
}

static HRESULT STDMETHODCALLTYPE hook_DevDrawIdxPrimStrided(IDirect3DDevice7 *this, D3DPRIMITIVETYPE t, DWORD fvf,
                                                            D3DDRAWPRIMITIVESTRIDEDDATA *d, DWORD n, WORD *idx,
                                                            DWORD ni, DWORD f) {
  g_drew_3d = 1;
  InterlockedIncrement(&g_draws);
  vb_check("DrawIndexedPrimitiveStrided", n, 0, n, idx, ni);
  return orig_DevDrawIdxPrimStrided(this, t, fvf, d, n, idx, ni, f);
}

static HRESULT STDMETHODCALLTYPE hook_DevDrawPrimVB(IDirect3DDevice7 *this, D3DPRIMITIVETYPE t,
                                                    IDirect3DVertexBuffer7 *vb, DWORD start, DWORD n, DWORD f) {
  g_drew_3d = 1;
  InterlockedIncrement(&g_draws);
  vb_check("DrawPrimitiveVB", vb_size(vb), start, n, NULL, 0);
  return orig_DevDrawPrimVB(this, t, vb, start, n, f);
}

static HRESULT STDMETHODCALLTYPE hook_DevDrawIdxPrimVB(IDirect3DDevice7 *this, D3DPRIMITIVETYPE t,
                                                       IDirect3DVertexBuffer7 *vb, DWORD start, DWORD n, WORD *idx,
                                                       DWORD ni, DWORD f) {
  g_drew_3d = 1;
  InterlockedIncrement(&g_draws);
  vb_check("DrawIndexedPrimitiveVB", vb_size(vb), start, n, idx, ni);
  return orig_DevDrawIdxPrimVB(this, t, vb, start, n, idx, ni, f);
}

static void wrap_d3ddev7(void *obj) {
  void **vt;
  if (!obj)
    return;
  vt = writable_vt(obj, 96);
  if (!vt)
    return;
  if (!orig_DevGetCaps)
    orig_DevGetCaps = (void *)vt[3];
  if (!orig_DevBeginScene)
    orig_DevBeginScene = (void *)vt[5];
  if (!orig_DevEndScene)
    orig_DevEndScene = (void *)vt[6];
  if (!orig_DevSetRT)
    orig_DevSetRT = (void *)vt[8];
  if (!orig_DevClear)
    orig_DevClear = (void *)vt[10];
  if (!orig_DevEnumTex)
    orig_DevEnumTex = (void *)vt[4];
  if (!orig_DevGetRT)
    orig_DevGetRT = (void *)vt[9];
  if (!orig_DevSetTransform)
    orig_DevSetTransform = (void *)vt[11];
  if (!orig_DevSetViewport)
    orig_DevSetViewport = (void *)vt[13];
  if (!orig_DevSetRS)
    orig_DevSetRS = (void *)vt[20];
  vt[3] = (void *)hook_DevGetCaps;
  vt[5] = (void *)hook_DevBeginScene;
  vt[6] = (void *)hook_DevEndScene;
  vt[8] = (void *)hook_DevSetRT;
  vt[10] = (void *)hook_DevClear;
  vt[4] = (void *)hook_DevEnumTex;
  vt[9] = (void *)hook_DevGetRT;
  vt[11] = (void *)hook_DevSetTransform;
  vt[13] = (void *)hook_DevSetViewport;
  vt[20] = (void *)hook_DevSetRS;
  if (!orig_DevDrawPrim)
    orig_DevDrawPrim = (void *)vt[25];
  if (!orig_DevDrawIdxPrim)
    orig_DevDrawIdxPrim = (void *)vt[26];
  if (!orig_DevDrawPrimStrided)
    orig_DevDrawPrimStrided = (void *)vt[29];
  if (!orig_DevDrawIdxPrimStrided)
    orig_DevDrawIdxPrimStrided = (void *)vt[30];
  if (!orig_DevDrawPrimVB)
    orig_DevDrawPrimVB = (void *)vt[31];
  if (!orig_DevDrawIdxPrimVB)
    orig_DevDrawIdxPrimVB = (void *)vt[32];
  vt[25] = (void *)hook_DevDrawPrim;
  vt[26] = (void *)hook_DevDrawIdxPrim;
  vt[29] = (void *)hook_DevDrawPrimStrided;
  vt[30] = (void *)hook_DevDrawIdxPrimStrided;
  vt[31] = (void *)hook_DevDrawPrimVB;
  vt[32] = (void *)hook_DevDrawIdxPrimVB;
  ee_log("wrapped IDirect3DDevice7 vtable %p", obj);
  log_null_slots("IDirect3DDevice7", vt, 49);
}


static HRESULT(STDMETHODCALLTYPE *orig_D3DCreateVB)(IDirect3D7 *, D3DVERTEXBUFFERDESC *, IDirect3DVertexBuffer7 **,
                                                    DWORD);

static HRESULT STDMETHODCALLTYPE hook_D3DCreateVB(IDirect3D7 *this, D3DVERTEXBUFFERDESC *desc,
                                                  IDirect3DVertexBuffer7 **vb, DWORD flags) {
  HRESULT hr = orig_D3DCreateVB ? orig_D3DCreateVB(this, desc, vb, flags) : DDERR_GENERIC;
  if (SUCCEEDED(hr) && vb && *vb && desc) {
    LONG i = InterlockedIncrement(&g_nvbs) - 1;
    if (i < VB_TABLE) {
      g_vbs[i].vb = *vb;
      g_vbs[i].verts = desc->dwNumVertices;
    }
    if (i < 40)
      ee_log("vb: created %p caps=0x%lx fvf=0x%lx vertices=%lu", (void *)*vb, (unsigned long)desc->dwCaps,
             (unsigned long)desc->dwFVF, (unsigned long)desc->dwNumVertices);
  }
  return hr;
}

static void wrap_d3d7(void *obj) {
  void **vt;
  if (!obj)
    return;
  vt = writable_vt(obj, 32);
  if (!vt)
    return;
  if (!orig_D3DEnumDevices)
    orig_D3DEnumDevices = (void *)vt[3];
  if (!orig_D3DCreateDevice)
    orig_D3DCreateDevice = (void *)vt[4];
  if (!orig_D3DEnumZBuffer)
    orig_D3DEnumZBuffer = (void *)vt[6];
  if (!orig_D3DCreateVB)
    orig_D3DCreateVB = (void *)vt[5];
  vt[5] = (void *)hook_D3DCreateVB;
  vt[3] = (void *)hook_D3DEnumDevices;
  vt[4] = (void *)hook_D3DCreateDevice;
  vt[6] = (void *)hook_D3DEnumZBuffer;
  ee_log("wrapped IDirect3D7 vtable %p", obj);
  log_null_slots("IDirect3D7", vt, 8);
}

static HRESULT STDMETHODCALLTYPE hook_QI7(IDirectDraw7 *this, REFIID riid, void **out) {
  HRESULT hr = orig_QI7 ? orig_QI7(this, riid, out) : E_NOINTERFACE;
  const char *what = "other";
  if (riid && IsEqualGUID(riid, &IID_IDirect3D7))
    what = "IID_IDirect3D7";
  ee_log("IDirectDraw7::QueryInterface %s (%s) hr=0x%08lx", what, guid_str(riid), (unsigned long)hr);
  if (SUCCEEDED(hr) && out && *out && riid && IsEqualGUID(riid, &IID_IDirect3D7))
    wrap_d3d7(*out);
  return hr;
}

/* NOTE: hooking IDirectDraw7 slots 23 (GetAvailableVidMem), 26
 * (TestCooperativeLevel) and 27 (GetDeviceIdentifier) was tried on 16 Sep to see
 * whether the game silently quits over a zero-video-memory report. Installing
 * those three hooks -- even log-only, with correct signatures -- makes the game
 * die inside DirectDrawEnumerateExA. Slots 28/29 are hooked safely, so this is
 * not a simple out-of-range problem; something about D7VK's vtable does not
 * tolerate it. Do not re-add without a different approach (e.g. wrapping the
 * object rather than patching its vtable).
 */

/* ---- the four surface slots that were never hooked -------------------------
 * Until 16 Sep the investigation could not distinguish "the game stopped
 * calling DirectDraw" from "the game is blocked inside a call we do not see".
 * BltFast (7), GetBltStatus (13), GetFlipStatus (18) and Unlock (32) are all
 * plausible sites: the classic 2001 present idiom is
 *     while (GetFlipStatus(DDGFS_ISFLIPDONE) == DDERR_WASSTILLDRAWING) ;
 * which, if D7VK never clears the status, spins with zero further logged calls
 * -- indistinguishable from the stall we see.  Unlock is where the texture
 * upload actually happens, so a block there would look identical too.
 *
 * The handoff's warning about slots 23/26/27 is about the IDirectDraw7 vtable,
 * not this one; log_null_slots() below verifies the wrap. */
static HRESULT(STDMETHODCALLTYPE *orig_SurfBltFast)(IDirectDrawSurface7 *, DWORD, DWORD, IDirectDrawSurface7 *, LPRECT,
                                                    DWORD);
static HRESULT(STDMETHODCALLTYPE *orig_SurfUnlock)(IDirectDrawSurface7 *, LPRECT);
static HRESULT(STDMETHODCALLTYPE *orig_SurfGetBltStatus)(IDirectDrawSurface7 *, DWORD);
static HRESULT(STDMETHODCALLTYPE *orig_SurfGetFlipStatus)(IDirectDrawSurface7 *, DWORD);

static HRESULT STDMETHODCALLTYPE hook_SurfBltFast(IDirectDrawSurface7 *this, DWORD x, DWORD y,
                                                  IDirectDrawSurface7 *src, LPRECT sr, DWORD trans) {
  static int n;
  HRESULT hr;
  if (n++ < 200)
    ee_log("Surface::BltFast ENTER dst=%p src=%p x=%lu y=%lu trans=0x%lx", (void *)this, (void *)src,
           (unsigned long)x, (unsigned long)y, (unsigned long)trans);
  hr = orig_SurfBltFast ? orig_SurfBltFast(this, x, y, src, sr, trans) : DDERR_GENERIC;
  if (n < 200)
    ee_log("Surface::BltFast hr=0x%08lx", (unsigned long)hr);
  return hr;
}

static HRESULT STDMETHODCALLTYPE hook_SurfUnlock(IDirectDrawSurface7 *this, LPRECT r) {
  HRESULT hr;
  static unsigned n;
  static unsigned n_fail; /* failures get their own budget */
  int loud = hot_ok(&n);
  if (loud)
    ee_log("Surface::Unlock ENTER this=%p", (void *)this);
  hr = orig_SurfUnlock ? orig_SurfUnlock(this, r) : DDERR_GENERIC;
  if (loud || (FAILED(hr) && hot_ok(&n_fail)))
    ee_log("Surface::Unlock hr=0x%08lx", (unsigned long)hr);
  return hr;
}

/* A polling loop will hammer these, so the RATE is the signal: log the first
 * 200, then every 5000th. */
static HRESULT STDMETHODCALLTYPE hook_SurfGetBltStatus(IDirectDrawSurface7 *this, DWORD flags) {
  static int n;
  HRESULT hr = orig_SurfGetBltStatus ? orig_SurfGetBltStatus(this, flags) : DDERR_GENERIC;
  if (n++ < 200 || (n % 5000) == 0)
    ee_log("Surface::GetBltStatus this=%p flags=0x%lx hr=0x%08lx (#%d)", (void *)this, (unsigned long)flags,
           (unsigned long)hr, n);
  return hr;
}

static HRESULT STDMETHODCALLTYPE hook_SurfGetFlipStatus(IDirectDrawSurface7 *this, DWORD flags) {
  static int n;
  HRESULT hr = orig_SurfGetFlipStatus ? orig_SurfGetFlipStatus(this, flags) : DDERR_GENERIC;
  if (n++ < 200 || (n % 5000) == 0)
    ee_log("Surface::GetFlipStatus this=%p flags=0x%lx hr=0x%08lx (#%d)", (void *)this, (unsigned long)flags,
           (unsigned long)hr, n);
  return hr;
}

static void wrap_surf7(void *obj) {
  void **vt;
  if (!obj)
    return;
  vt = writable_vt(obj, 96);
  if (!vt)
    return;
  if (!orig_SurfBlt)
    orig_SurfBlt = (void *)vt[5];
  if (!orig_SurfGetDC)
    orig_SurfGetDC = (void *)vt[17];
  if (!orig_SurfQI)
    orig_SurfQI = (void *)vt[0];
  if (!orig_SurfFlip)
    orig_SurfFlip = (void *)vt[11];
  if (!orig_SurfGetAttached)
    orig_SurfGetAttached = (void *)vt[12];
  if (!orig_SurfLock)
    orig_SurfLock = (void *)vt[25];
  if (!orig_SurfBltFast)
    orig_SurfBltFast = (void *)vt[7];
  if (!orig_SurfGetBltStatus)
    orig_SurfGetBltStatus = (void *)vt[13];
  if (!orig_SurfGetFlipStatus)
    orig_SurfGetFlipStatus = (void *)vt[18];
  if (!orig_SurfUnlock)
    orig_SurfUnlock = (void *)vt[32];
  if (!orig_SurfReleaseDC)
    orig_SurfReleaseDC = (void *)vt[26];
  vt[5] = (void *)hook_SurfBlt;
  vt[17] = (void *)hook_SurfGetDC;
  vt[0] = (void *)hook_SurfQI;
  vt[11] = (void *)hook_SurfFlip;
  vt[12] = (void *)hook_SurfGetAttached;
  vt[25] = (void *)hook_SurfLock;
  /* The four extra slots are opt-in: EE_DDRAW_EXTRA_HOOKS is a bitmask,
   * 1=BltFast(7) 2=GetBltStatus(13) 4=GetFlipStatus(18) 8=Unlock(32), so each
   * can be bisected without a rebuild.  They are off by default until one is
   * proven safe -- this vtable has already shown it does not tolerate every
   * slot being patched. */
  {
    static int extra = -1;
    if (extra < 0) {
      char buf[16];
      extra = (GetEnvironmentVariableA("EE_DDRAW_EXTRA_HOOKS", buf, sizeof buf) > 0) ? atoi(buf) : 0;
    }
    if (extra & 16)
      vt[26] = (void *)hook_SurfReleaseDC; /* timing only */
    if (extra & 1)
      vt[7] = (void *)hook_SurfBltFast;
    if (extra & 2)
      vt[13] = (void *)hook_SurfGetBltStatus;
    if (extra & 4)
      vt[18] = (void *)hook_SurfGetFlipStatus;
    if (extra & 8)
      vt[32] = (void *)hook_SurfUnlock;
    {
      static unsigned n;
      if (hot_ok(&n))
        ee_log("wrapped surface %p (extra hooks=%d)", obj, extra);
    }
  }
  {
    static unsigned n;
    if (hot_ok(&n))
      log_null_slots("IDirectDrawSurface7", vt, 50);
  }
}

static void wrap_ddraw7(void *obj) {
  void **vt;
  if (!obj)
    return;
  vt = writable_vt(obj, 64);
  if (!vt)
    return;
  if (!orig_QI7)
    orig_QI7 = (void *)vt[0];
  if (!orig_CreateSurface7)
    orig_CreateSurface7 = (void *)vt[6];
  if (!orig_EnumDisplayModes7)
    orig_EnumDisplayModes7 = (void *)vt[8];
  if (!orig_GetCaps7)
    orig_GetCaps7 = (void *)vt[11];
  if (!orig_GetDisplayMode7)
    orig_GetDisplayMode7 = (void *)vt[12];
  if (!orig_GetScanLine7)
    orig_GetScanLine7 = (void *)vt[16];
  if (!orig_GetVBlankStatus7)
    orig_GetVBlankStatus7 = (void *)vt[17];
  if (!orig_SetCoop7)
    orig_SetCoop7 = (void *)vt[20];
  if (!orig_SetDisplayMode7)
    orig_SetDisplayMode7 = (void *)vt[21];
  if (!orig_WaitVBlank7)
    orig_WaitVBlank7 = (void *)vt[22];
  if (!orig_StartModeTest7)
    orig_StartModeTest7 = (void *)vt[28];
  if (!orig_EvaluateMode7)
    orig_EvaluateMode7 = (void *)vt[29];
  vt[0] = (void *)hook_QI7;
  vt[6] = (void *)hook_CreateSurface7;
  vt[8] = (void *)hook_EnumDisplayModes7;
  vt[11] = (void *)hook_GetCaps7;
  vt[12] = (void *)hook_GetDisplayMode7;
  vt[16] = (void *)hook_GetScanLine7;
  vt[17] = (void *)hook_GetVBlankStatus7;
  vt[20] = (void *)hook_SetCoop7;
  vt[21] = (void *)hook_SetDisplayMode7;
  vt[22] = (void *)hook_WaitVBlank7;
  vt[28] = (void *)hook_StartModeTest7;
  vt[29] = (void *)hook_EvaluateMode7;
  ee_log("wrapped IDirectDraw7 vtable");
  log_null_slots("IDirectDraw7", vt, 30);
}

static void ee_log(const char *fmt, ...) {
  va_list ap;
  /* Seconds since the proxy's first line, and the calling thread: a stall
   * inside one call is only visible as a gap in time, and a game thread parked
   * in its own window procedure only by its thread id. */
  static DWORD t0;
  char pre[40];
  DWORD now = GetTickCount();
  if (!t0)
    t0 = now;
  snprintf(pre, sizeof pre, "ee-ddraw: %lu.%03lu t%04lx ", (unsigned long)((now - t0) / 1000),
           (unsigned long)((now - t0) % 1000), (unsigned long)GetCurrentThreadId());
  if (!g_log) {
    g_log = fopen("ee-ddraw.log", "w");
    if (g_log)
      setvbuf(g_log, NULL, _IONBF, 0);
  }
  fputs(pre, stderr);
  if (g_log)
    fputs(pre, g_log);
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  va_start(ap, fmt);
  if (g_log)
    vfprintf(g_log, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  if (g_log)
    fputc('\n', g_log);
}

static void fill_mode(DEVMODEA *dm) {
  if (!dm)
    return;
  dm->dmFields |= DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
  dm->dmPelsWidth = 800;
  dm->dmPelsHeight = 600;
  dm->dmBitsPerPel = 32;
  dm->dmDisplayFrequency = 60;
}

static BOOL WINAPI hook_EnumDisplaySettingsA(const char *dev, DWORD mode, DEVMODEA *dm) {
  (void)dev;
  if (!dm)
    return FALSE;
  if (mode == ENUM_CURRENT_SETTINGS || mode == ENUM_REGISTRY_SETTINGS || mode <= 2) {
    fill_mode(dm);
    ee_log("EnumDisplaySettingsA %lu -> 800x600", (unsigned long)mode);
    return TRUE;
  }
  return FALSE;
}

static BOOL WINAPI hook_EnumDisplaySettingsExA(const char *dev, DWORD mode, DEVMODEA *dm, DWORD flags) {
  (void)flags;
  return hook_EnumDisplaySettingsA(dev, mode, dm);
}

static BOOL WINAPI hook_EnumDisplaySettingsW(const wchar_t *dev, DWORD mode, DEVMODEW *dm) {
  (void)dev;
  if (!dm)
    return FALSE;
  if (mode == ENUM_CURRENT_SETTINGS || mode == ENUM_REGISTRY_SETTINGS || mode <= 2) {
    dm->dmFields |= DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    dm->dmPelsWidth = 800;
    dm->dmPelsHeight = 600;
    dm->dmBitsPerPel = 32;
    dm->dmDisplayFrequency = 60;
    ee_log("EnumDisplaySettingsW %lu -> 800x600", (unsigned long)mode);
    return TRUE;
  }
  return FALSE;
}

static BOOL WINAPI hook_EnumDisplaySettingsExW(const wchar_t *dev, DWORD mode, DEVMODEW *dm, DWORD flags) {
  (void)flags;
  return hook_EnumDisplaySettingsW(dev, mode, dm);
}

static LONG WINAPI hook_ChangeDisplaySettingsA(DEVMODEA *dm, DWORD flags) {
  (void)dm;
  (void)flags;
  ee_log("ChangeDisplaySettingsA SUCCESS");
  return DISP_CHANGE_SUCCESSFUL;
}

static LONG WINAPI hook_ChangeDisplaySettingsW(DEVMODEW *dm, DWORD flags) {
  (void)dm;
  (void)flags;
  return DISP_CHANGE_SUCCESSFUL;
}

static LONG WINAPI hook_ChangeDisplaySettingsExA(const char *d, DEVMODEA *m, HWND h, DWORD f, void *p) {
  (void)d;
  (void)m;
  (void)h;
  (void)f;
  (void)p;
  return DISP_CHANGE_SUCCESSFUL;
}

static LONG WINAPI hook_ChangeDisplaySettingsExW(const wchar_t *d, DEVMODEW *m, HWND h, DWORD f, void *p) {
  (void)d;
  (void)m;
  (void)h;
  (void)f;
  (void)p;
  return DISP_CHANGE_SUCCESSFUL;
}

static int WINAPI hook_GetSystemMetrics(int idx) {
  if (idx == SM_CXSCREEN || idx == SM_CXFULLSCREEN || idx == SM_CXVIRTUALSCREEN)
    return 800;
  if (idx == SM_CYSCREEN || idx == SM_CYFULLSCREEN || idx == SM_CYVIRTUALSCREEN)
    return 600;
  return orig_GetSystemMetrics ? orig_GetSystemMetrics(idx) : 800;
}

static void *rva_ptr(HMODULE mod, DWORD rva) { return (char *)mod + rva; }

static int patch_iat(HMODULE mod, const char *dllwant, const char *fn, void *hook) {
  IMAGE_DOS_HEADER *dos;
  IMAGE_NT_HEADERS *nt;
  IMAGE_IMPORT_DESCRIPTOR *imp;
  DWORD imp_rva;
  if (!mod)
    return 0;
  dos = (IMAGE_DOS_HEADER *)mod;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    return 0;
  nt = (IMAGE_NT_HEADERS *)rva_ptr(mod, (DWORD)dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE)
    return 0;
  imp_rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
  if (!imp_rva)
    return 0;
  for (imp = rva_ptr(mod, imp_rva); imp->Name; imp++) {
    IMAGE_THUNK_DATA *orig;
    IMAGE_THUNK_DATA *th;
    const char *dll = rva_ptr(mod, imp->Name);
    if (_stricmp(dll, dllwant) != 0)
      continue;
    orig = rva_ptr(mod, imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk);
    th = rva_ptr(mod, imp->FirstThunk);
    for (; orig->u1.AddressOfData; orig++, th++) {
      IMAGE_IMPORT_BY_NAME *by;
      DWORD old;
      if (IMAGE_SNAP_BY_ORDINAL(orig->u1.Ordinal))
        continue;
      by = rva_ptr(mod, orig->u1.AddressOfData);
      if (strcmp((char *)by->Name, fn) != 0)
        continue;
      if (!VirtualProtect(&th->u1.Function, sizeof(th->u1.Function), PAGE_READWRITE, &old))
        return 0;
      th->u1.Function = (ULONG_PTR)hook;
      VirtualProtect(&th->u1.Function, sizeof(th->u1.Function), old, &old);
      ee_log("IAT %s!%s", dll, fn);
      return 1;
    }
  }
  return 0;
}

static void patch_one(HMODULE mod) {
  if (!mod)
    return;
  patch_iat(mod, "user32.dll", "EnumDisplaySettingsA", (void *)hook_EnumDisplaySettingsA);
  patch_iat(mod, "user32.dll", "EnumDisplaySettingsExA", (void *)hook_EnumDisplaySettingsExA);
  patch_iat(mod, "user32.dll", "EnumDisplaySettingsW", (void *)hook_EnumDisplaySettingsW);
  patch_iat(mod, "user32.dll", "EnumDisplaySettingsExW", (void *)hook_EnumDisplaySettingsExW);
  patch_iat(mod, "user32.dll", "ChangeDisplaySettingsA", (void *)hook_ChangeDisplaySettingsA);
  patch_iat(mod, "user32.dll", "ChangeDisplaySettingsW", (void *)hook_ChangeDisplaySettingsW);
  patch_iat(mod, "user32.dll", "ChangeDisplaySettingsExA", (void *)hook_ChangeDisplaySettingsExA);
  patch_iat(mod, "user32.dll", "ChangeDisplaySettingsExW", (void *)hook_ChangeDisplaySettingsExW);
  patch_iat(mod, "user32.dll", "GetSystemMetrics", (void *)hook_GetSystemMetrics);
}

/* Focus loss must not minimize the game.  Wine's wined3d and DXVK's d3d9 both
 * emulate Windows' exclusive-fullscreen behaviour: on WM_ACTIVATEAPP(FALSE)
 * they ShowWindow(SW_MINIMIZE) the device window (wined3d_swapchain_activate;
 * Wine's ddraw never passes WINED3DCREATE_NOWINDOWCHANGES, so DDSCL_NOWINDOWCHANGES
 * cannot stop it).  An iconic window then makes the D3D9 device report lost,
 * D7VK turns that into DDERR_SURFACELOST on every Flip, and mid-match Empire
 * Earth never recovers: one Cmd-Tab and the screen stays black for good (22 Sep
 * 2026).  So veto that one call, for that one window, in the modules that make
 * it.  Ctrl+Alt+Q still minimizes: the watchdog calls ShowWindow through our own
 * import table, which is not patched.  EE_DDRAW_ALLOW_MINIMIZE=1 turns this off. */
static BOOL(WINAPI *orig_ShowWindow)(HWND, int);

static BOOL WINAPI hook_ShowWindow(HWND hwnd, int cmd) {
  if (hwnd && hwnd == g_excl_hwnd && !g_handsoff &&
      (cmd == SW_MINIMIZE || cmd == SW_SHOWMINIMIZED || cmd == SW_SHOWMINNOACTIVE || cmd == SW_FORCEMINIMIZE)) {
    static LONG vetoes;
    LONG n = InterlockedIncrement(&vetoes);
    if (n <= 5 || n % 100 == 0)
      ee_log("ShowWindow(%p, %d) -- minimize on focus loss vetoed (#%ld)", (void *)hwnd, cmd, (long)n);
    return IsWindowVisible(hwnd);
  }
  return orig_ShowWindow ? orig_ShowWindow(hwnd, cmd) : FALSE;
}

/* The real fix for "one Cmd-Tab and the match stays black".  D7VK passes
 * SetCooperativeLevel through to Wine's builtin ddraw (ddraw_.dll), which on
 * DDSCL_EXCLUSIVE hooks the window's messages with
 * wined3d_device_acquire_focus_window (Wine 11 ddraw.c:985).  On
 * WM_ACTIVATEAPP(FALSE) that hook marks ddraw's device LOST (and minimizes the
 * window); coming back only moves it to NOT_RESTORED, and every Flip then fails
 * with DDERR_SURFACELOST until the game calls Restore -- which Empire Earth never
 * does mid-match.  DXVK-Sarek itself never reports a lost device.  So answer the
 * call without installing the hook: ddraw then never hears about focus changes,
 * nothing is marked lost, nothing is minimized, and no display mode is dropped
 * and re-applied behind the game's back.  Patched from hook_SetCoop7, on the
 * game's thread, just before the call that would install the hook.
 * EE_DDRAW_FOCUS_HOOK=1 keeps Wine's behaviour. */
static HRESULT __cdecl hook_acquire_focus_window(void *device, HWND window) {
  static LONG n;
  (void)device;
  if (InterlockedIncrement(&n) <= 5)
    ee_log("wined3d focus hook for %p declined -- focus changes no longer lose surfaces", (void *)window);
  return S_OK;
}

static void __cdecl hook_release_focus_window(void *device) { (void)device; }

static void bypass_wined3d_focus_hook(void) {
  static const char *mods[] = { "ddraw_.dll", "ee_sysddraw.dll" };
  static HMODULE done[sizeof(mods) / sizeof(mods[0])];
  static int enabled = -1;
  size_t i;
  if (enabled < 0) {
    char b[8];
    enabled = !(GetEnvironmentVariableA("EE_DDRAW_FOCUS_HOOK", b, sizeof b) > 0 && b[0] == '1');
  }
  if (!enabled)
    return;
  for (i = 0; i < sizeof(mods) / sizeof(mods[0]); i++) {
    HMODULE m = GetModuleHandleA(mods[i]);
    if (m && m != done[i]) {
      patch_iat(m, "wined3d.dll", "wined3d_device_acquire_focus_window", (void *)hook_acquire_focus_window);
      patch_iat(m, "wined3d.dll", "wined3d_device_release_focus_window", (void *)hook_release_focus_window);
      done[i] = m;
    }
  }
}

/* Empire Earth itself minimizes its window on deactivation too, with
 * CloseWindow -- which in Win32 means "minimize", not "close".  On a Mac that
 * parks the game in the Dock and Cmd-Tab back does not bring it out, so veto
 * that as well: the game stays behind whatever app you switched to, intact. */
static BOOL(WINAPI *orig_CloseWindow)(HWND);

static BOOL WINAPI hook_CloseWindow(HWND hwnd) {
  if (hwnd && hwnd == g_excl_hwnd && !g_handsoff) {
    static LONG vetoes;
    LONG n = InterlockedIncrement(&vetoes);
    if (n <= 5 || n % 100 == 0)
      ee_log("CloseWindow(%p) -- the game minimizing itself on focus loss, vetoed (#%ld)", (void *)hwnd, (long)n);
    return TRUE;
  }
  return orig_CloseWindow ? orig_CloseWindow(hwnd) : FALSE;
}

/* The wrappers load late (d3d9.dll with the first device) and can be unloaded
 * and reloaded, so the watchdog calls this every second; a module is patched
 * once per load address (NULL = the game's own exe).  Never called under the
 * loader lock. */
static void veto_focus_loss_minimize(void) {
  static const char *mods[] = { NULL, "d3d9.dll", "wined3d.dll", "ddraw_.dll", "ee_sysddraw.dll" };
  static HMODULE done[sizeof(mods) / sizeof(mods[0])];
  static int enabled = -1;
  size_t i;
  if (enabled < 0) {
    char b[8];
    enabled = !(GetEnvironmentVariableA("EE_DDRAW_ALLOW_MINIMIZE", b, sizeof b) > 0 && b[0] == '1');
    if (enabled) {
      HMODULE user = GetModuleHandleA("user32.dll");
      if (user && !orig_ShowWindow)
        orig_ShowWindow = (void *)GetProcAddress(user, "ShowWindow");
      if (user && !orig_CloseWindow)
        orig_CloseWindow = (void *)GetProcAddress(user, "CloseWindow");
    }
    if (!orig_ShowWindow || !orig_CloseWindow)
      enabled = 0;
  }
  if (!enabled)
    return;
  for (i = 0; i < sizeof(mods) / sizeof(mods[0]); i++) {
    HMODULE m = GetModuleHandleA(mods[i]);
    if (m && m != done[i]) {
      int a = patch_iat(m, "user32.dll", "ShowWindow", (void *)hook_ShowWindow);
      int b = patch_iat(m, "user32.dll", "CloseWindow", (void *)hook_CloseWindow);
      if (a || b)
        ee_log("minimize-on-focus-loss veto installed in %s", mods[i] ? mods[i] : "the game exe");
      done[i] = m;
    }
  }
}

static void install_mode_hooks(void) {
  static int once;
  HMODULE user;
  if (once)
    return;
  once = 1;
  user = GetModuleHandleA("user32.dll");
  if (user)
    orig_GetSystemMetrics = (void *)GetProcAddress(user, "GetSystemMetrics");
  patch_one(GetModuleHandleA(NULL));
}

/* D7VK proxies DirectDrawCreateEx / DirectDrawEnumerate* to "<sysdir>\\ddraw.dll"
 * to get a real DirectDraw object to wrap. Under Wine the loader keys modules by
 * BASE NAME, so that request comes straight back to us and D7VK logs
 * "Failed to load proxied ddraw.dll". ee_sysddraw.dll is a copy of Wine's
 * builtin syswow64\ddraw.dll under a distinct module name, so we can serve those
 * re-entrant requests with a genuine implementation instead of recursing. */
static HMODULE g_sysdd;

static int load_sysdd(void) {
  char path[MAX_PATH];
  char *slash;
  if (g_sysdd)
    return 1;
  GetModuleFileNameA((HINSTANCE)&__ImageBase, path, MAX_PATH);
  slash = strrchr(path, '\\');
  if (!slash)
    slash = strrchr(path, '/');
  if (!slash)
    return 0;
  strcpy(slash + 1, "ee_sysddraw.dll");
  g_sysdd = LoadLibraryA(path);
  if (!g_sysdd) {
    ee_log("ee_sysddraw.dll load failed %lu", (unsigned long)GetLastError());
    return 0;
  }
  ee_log("loaded builtin ddraw copy %s", path);
  return 1;
}

/* ---- Vulkan surface size under Wine's emulated display modes ---------------
 * With EmulateModeset (EE_EMULATE_MODESET=1) the game's fixed 1024x768 menu is a
 * full-screen window at an emulated mode, which Wine lays out scaled up to the
 * display (1312x984 on 1512x982).  While DXVK sets up its swapchain, winevulkan
 * reports that scaled size as the surface's currentExtent -- but at every present
 * it compares the swapchain with the plain 1024x768 client rect and returns
 * VK_SUBOPTIMAL_KHR, and DXVK-Sarek recreates its swapchain after every
 * suboptimal present (21,020 times in one short run, 22 Sep 2026) while its blit
 * lands 1:1 in the top-left of the bigger images.  So report the game window's
 * client rect as its surface size: DXVK then uses 1024x768 swapchains that agree
 * with its own blit and with winevulkan's check; MoltenVK's own "smaller than the
 * layer" suboptimal is swallowed by ee-vkfix, and Core Animation scales the
 * picture up to fill the screen.  Wrapped through winevulkan's export of
 * vkGetInstanceProcAddr, patched before D7VK loads DXVK. */
typedef struct {
  uint32_t width, height;
} ee_VkExtent2D;
typedef struct {
  uint32_t minImageCount, maxImageCount;
  ee_VkExtent2D currentExtent, minImageExtent, maxImageExtent;
  uint32_t maxImageArrayLayers, supportedTransforms, currentTransform, supportedCompositeAlpha, supportedUsageFlags;
} ee_VkSurfaceCapabilitiesKHR;
typedef struct {
  uint32_t sType;
  void *pNext;
  ee_VkSurfaceCapabilitiesKHR surfaceCapabilities;
} ee_VkSurfaceCapabilities2KHR;
typedef struct {
  uint32_t sType;
  const void *pNext;
  uint64_t surface;
} ee_VkPhysicalDeviceSurfaceInfo2KHR;
typedef struct {
  uint32_t sType;
  const void *pNext;
  uint32_t flags;
  HINSTANCE hinstance;
  HWND hwnd;
} ee_VkWin32SurfaceCreateInfoKHR;
typedef void(__stdcall *ee_PFN_vkVoidFunction)(void);
typedef ee_PFN_vkVoidFunction(__stdcall *ee_PFN_vkGetInstanceProcAddr)(void *, const char *);
typedef int32_t(__stdcall *ee_PFN_vkCreateWin32SurfaceKHR)(void *, const ee_VkWin32SurfaceCreateInfoKHR *,
                                                            const void *, uint64_t *);
typedef int32_t(__stdcall *ee_PFN_vkGetSurfaceCaps)(void *, uint64_t, ee_VkSurfaceCapabilitiesKHR *);
typedef int32_t(__stdcall *ee_PFN_vkGetSurfaceCaps2)(void *, const ee_VkPhysicalDeviceSurfaceInfo2KHR *,
                                                     ee_VkSurfaceCapabilities2KHR *);

static ee_PFN_vkGetInstanceProcAddr real_vkGetInstanceProcAddr;
static ee_PFN_vkCreateWin32SurfaceKHR real_vkCreateWin32SurfaceKHR;
static ee_PFN_vkGetSurfaceCaps real_vkGetSurfaceCaps;
static ee_PFN_vkGetSurfaceCaps2 real_vkGetSurfaceCaps2;
static struct {
  uint64_t surface;
  HWND hwnd;
} g_vk_surfaces[64];
static LONG g_vk_nsurfaces;

static HWND vk_surface_hwnd(uint64_t surface) {
  LONG i, n = g_vk_nsurfaces < 64 ? g_vk_nsurfaces : 64;
  for (i = 0; i < n; i++)
    if (g_vk_surfaces[i].surface == surface)
      return g_vk_surfaces[i].hwnd;
  return NULL;
}

static void vk_caps_to_client(uint64_t surface, ee_VkSurfaceCapabilitiesKHR *caps) {
  HWND hwnd = vk_surface_hwnd(surface);
  RECT rc;
  uint32_t w, h;
  if (!caps || !hwnd || hwnd != g_excl_hwnd || !GetClientRect(hwnd, &rc))
    return;
  w = rc.right - rc.left;
  h = rc.bottom - rc.top;
  if (!w || !h || (caps->currentExtent.width == w && caps->currentExtent.height == h))
    return;
  {
    static uint32_t last_from_w, last_to_w, last_to_h;
    if (caps->currentExtent.width != last_from_w || w != last_to_w || h != last_to_h)
      ee_log("Vulkan surface of hwnd=%p: reporting its client size %ux%u instead of the scaled %ux%u", (void *)hwnd,
             w, h, caps->currentExtent.width, caps->currentExtent.height);
    last_from_w = caps->currentExtent.width;
    last_to_w = w;
    last_to_h = h;
  }
  caps->currentExtent.width = w;
  caps->currentExtent.height = h;
  if (caps->minImageExtent.width > w)
    caps->minImageExtent.width = w;
  if (caps->minImageExtent.height > h)
    caps->minImageExtent.height = h;
  if (caps->maxImageExtent.width < w)
    caps->maxImageExtent.width = w;
  if (caps->maxImageExtent.height < h)
    caps->maxImageExtent.height = h;
}

static int32_t __stdcall hook_vkCreateWin32SurfaceKHR(void *instance, const ee_VkWin32SurfaceCreateInfoKHR *info,
                                                      const void *alloc, uint64_t *surface) {
  int32_t r = real_vkCreateWin32SurfaceKHR(instance, info, alloc, surface);
  if (r == 0 && info && surface) {
    LONG i = InterlockedIncrement(&g_vk_nsurfaces) - 1;
    g_vk_surfaces[i % 64].hwnd = info->hwnd;
    g_vk_surfaces[i % 64].surface = *surface;
  }
  return r;
}

static int32_t __stdcall hook_vkGetSurfaceCaps(void *gpu, uint64_t surface, ee_VkSurfaceCapabilitiesKHR *caps) {
  int32_t r = real_vkGetSurfaceCaps(gpu, surface, caps);
  if (r == 0)
    vk_caps_to_client(surface, caps);
  return r;
}

static int32_t __stdcall hook_vkGetSurfaceCaps2(void *gpu, const ee_VkPhysicalDeviceSurfaceInfo2KHR *info,
                                               ee_VkSurfaceCapabilities2KHR *caps) {
  int32_t r = real_vkGetSurfaceCaps2(gpu, info, caps);
  if (r == 0 && info && caps)
    vk_caps_to_client(info->surface, &caps->surfaceCapabilities);
  return r;
}

static ee_PFN_vkVoidFunction __stdcall hook_vkGetInstanceProcAddr(void *instance, const char *name) {
  ee_PFN_vkVoidFunction f = real_vkGetInstanceProcAddr(instance, name);
  if (!f || !name)
    return f;
  if (!strcmp(name, "vkGetInstanceProcAddr"))
    return (ee_PFN_vkVoidFunction)hook_vkGetInstanceProcAddr;
  if (!strcmp(name, "vkCreateWin32SurfaceKHR")) {
    real_vkCreateWin32SurfaceKHR = (ee_PFN_vkCreateWin32SurfaceKHR)f;
    return (ee_PFN_vkVoidFunction)hook_vkCreateWin32SurfaceKHR;
  }
  if (!strcmp(name, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")) {
    real_vkGetSurfaceCaps = (ee_PFN_vkGetSurfaceCaps)f;
    return (ee_PFN_vkVoidFunction)hook_vkGetSurfaceCaps;
  }
  if (!strcmp(name, "vkGetPhysicalDeviceSurfaceCapabilities2KHR")) {
    real_vkGetSurfaceCaps2 = (ee_PFN_vkGetSurfaceCaps2)f;
    return (ee_PFN_vkVoidFunction)hook_vkGetSurfaceCaps2;
  }
  return f;
}

/* Point a DLL's export at our function; returns the original, or NULL. */
static void *patch_export(HMODULE mod, const char *name, void *hook) {
  IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)mod;
  IMAGE_NT_HEADERS *nt;
  IMAGE_EXPORT_DIRECTORY *exp;
  DWORD *funcs, *names, i, old;
  WORD *ords;
  if (!mod || dos->e_magic != IMAGE_DOS_SIGNATURE)
    return NULL;
  nt = (IMAGE_NT_HEADERS *)rva_ptr(mod, (DWORD)dos->e_lfanew);
  if (!nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress)
    return NULL;
  exp = rva_ptr(mod, nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
  funcs = rva_ptr(mod, exp->AddressOfFunctions);
  names = rva_ptr(mod, exp->AddressOfNames);
  ords = rva_ptr(mod, exp->AddressOfNameOrdinals);
  for (i = 0; i < exp->NumberOfNames; i++) {
    DWORD *slot;
    void *orig;
    if (strcmp((const char *)rva_ptr(mod, names[i]), name))
      continue;
    slot = &funcs[ords[i]];
    orig = rva_ptr(mod, *slot);
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old))
      return NULL;
    *slot = (DWORD)((char *)hook - (char *)mod);
    VirtualProtect(slot, sizeof(*slot), old, &old);
    return orig;
  }
  return NULL;
}

/* Only winevulkan.dll, which DXVK loads first: vulkan-1.dll is the Khronos-style
 * loader, whose instances are not winevulkan's, so its entry points must not be
 * routed to winevulkan's.  Installed from the game's first DirectDraw call, not
 * from DllMain: loading winevulkan.dll while the process was still initialising
 * (the game imports ddraw.dll directly) overflowed the main thread's stack at
 * the first DirectDrawCreateEx, 3 of 3 launches on 22 Sep 2026. */
static void hook_vulkan_surface_size(void) {
  static LONG done;
  char b[8];
  void *orig;
  if (InterlockedExchange(&done, 1))
    return;
  if (!(GetEnvironmentVariableA("EE_EMULATE_MODESET", b, sizeof b) > 0 && b[0] == '1'))
    return;
  if (GetEnvironmentVariableA("EE_DDRAW_VK_CLIENT_EXTENT", b, sizeof b) > 0 && b[0] == '0')
    return;
  orig = patch_export(LoadLibraryA("winevulkan.dll"), "vkGetInstanceProcAddr", (void *)hook_vkGetInstanceProcAddr);
  if (orig)
    real_vkGetInstanceProcAddr = (ee_PFN_vkGetInstanceProcAddr)orig;
  ee_log("winevulkan.dll!vkGetInstanceProcAddr %s", orig ? "wrapped (surface size = client size)" : "not wrapped");
}

static int load_real(void) {
  char path[MAX_PATH];
  char *slash;
  if (g_real)
    return 1;
  GetModuleFileNameA((HINSTANCE)&__ImageBase, path, MAX_PATH);
  slash = strrchr(path, '\\');
  if (!slash)
    slash = strrchr(path, '/');
  if (!slash)
    return 0;
  strcpy(slash + 1, "ddraw_eeorig.dll");
  g_real = LoadLibraryA(path);
  if (!g_real) {
    ee_log("LoadLibrary %s failed %lu", path, (unsigned long)GetLastError());
    return 0;
  }
  ee_log("wrapping %s", path);
  install_mode_hooks();
  return 1;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved) {
  (void)inst;
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) {
    InitializeCriticalSection(&g_pages_cs);
    load_real();
  }
  return TRUE;
}

#define FWD(ret, name, args, params)                                                                           \
  ret WINAPI name args {                                                                                       \
    static ret(WINAPI *real) args;                                                                             \
    if (!load_real())                                                                                          \
      return (ret)E_FAIL;                                                                                      \
    if (!real)                                                                                                 \
      real = (void *)GetProcAddress(g_real, #name);                                                            \
    if (!real)                                                                                                 \
      return (ret)E_FAIL;                                                                                      \
    return real params;                                                                                        \
  }

HRESULT WINAPI DirectDrawCreate(GUID *a, LPDIRECTDRAW *b, IUnknown *c) {
  HRESULT(WINAPI * real)(GUID *, LPDIRECTDRAW *, IUnknown *);
  HRESULT hr;
  hook_vulkan_surface_size();
  if (!load_real())
    return DDERR_GENERIC;
  real = (void *)GetProcAddress(g_real, "DirectDrawCreate");
  ee_log("DirectDrawCreate");
  pages_release("DirectDrawCreate");
  hr = real ? real(a, b, c) : DDERR_GENERIC;
  ee_log("DirectDrawCreate hr=0x%08lx", (unsigned long)hr);
  return hr;
}

static int g_create_depth;

HRESULT WINAPI DirectDrawCreateEx(GUID *a, LPVOID *b, REFIID c, IUnknown *d) {
  HRESULT(WINAPI * real)(GUID *, LPVOID *, REFIID, IUnknown *);
  HRESULT hr;
  hook_vulkan_surface_size();
  if (g_create_depth > 0) {
    /* D7VK is asking the "system ddraw" for the object it wraps. */
    if (!load_sysdd())
      return DDERR_GENERIC;
    real = (void *)GetProcAddress(g_sysdd, "DirectDrawCreateEx");
    ee_log("DirectDrawCreateEx re-entry -> builtin ddraw");
    hr = real ? real(a, b, c, d) : DDERR_GENERIC;
    ee_log("DirectDrawCreateEx re-entry hr=0x%08lx", (unsigned long)hr);
    return hr;
  }
  if (!load_real())
    return DDERR_GENERIC;
  real = (void *)GetProcAddress(g_real, "DirectDrawCreateEx");
  ee_log("DirectDrawCreateEx");
  pages_release("DirectDrawCreateEx");
  g_create_depth++;
  hr = real ? real(a, b, c, d) : DDERR_GENERIC;
  g_create_depth--;
  ee_log("DirectDrawCreateEx hr=0x%08lx", (unsigned long)hr);
  if (SUCCEEDED(hr) && b && *b)
    wrap_ddraw7(*b);
  install_mode_hooks();
  return hr;
}

/* D7VK implements DirectDrawEnumerate* by proxying to the system ddraw.dll.
 * When D7VK *is* ddraw.dll next to the game, LoadLibrary("<sys>\\ddraw.dll")
 * returns the already-loaded module -- itself -- so it logs
 * "Failed to load proxied ddraw.dll" and enumerates nothing. Empire Earth then
 * works from an empty adapter list and dereferences NULL in
 * Low-Level Engine.dll+0x6FE8.
 *
 * So: forward first, and if the real module called the callback zero times,
 * synthesise the primary display entry ourselves -- exactly what a real
 * DirectDrawEnumerateEx does on a single-monitor machine. On dgVoodoo, which
 * enumerates properly, the counter is non-zero and nothing is synthesised.
 */
static int g_enum_seen;
/* With the proxy in front of D7VK, D7VK's own proxy loads "<sys>\\ddraw.dll",
 * gets *us*, and calls straight back in -- unbounded recursion that faults in
 * ~4s at 7BF21139. Answer a re-entrant call directly instead of forwarding. */
static int g_enum_depth;
static CHAR g_primary_desc[] = "Primary Display Driver";
static CHAR g_primary_name[] = "display";

typedef struct {
  LPDDENUMCALLBACKA cb;
  LPVOID ctx;
} enum_a_ctx;

typedef struct {
  LPDDENUMCALLBACKEXA cb;
  LPVOID ctx;
} enum_ex_ctx;

static BOOL WINAPI tramp_enum_a(GUID *guid, LPSTR desc, LPSTR name, LPVOID ctx) {
  enum_a_ctx *e = ctx;
  g_enum_seen++;
  ee_log("  adapter %s desc=\"%s\" name=\"%s\"", guid ? guid_str(guid) : "(primary)", desc ? desc : "",
         name ? name : "");
  return e->cb ? e->cb(guid, desc, name, e->ctx) : DDENUMRET_OK;
}

static BOOL WINAPI tramp_enum_ex(GUID *guid, LPSTR desc, LPSTR name, LPVOID ctx, HMONITOR mon) {
  enum_ex_ctx *e = ctx;
  g_enum_seen++;
  ee_log("  adapter %s desc=\"%s\" name=\"%s\"", guid ? guid_str(guid) : "(primary)", desc ? desc : "",
         name ? name : "");
  return e->cb ? e->cb(guid, desc, name, e->ctx, mon) : DDENUMRET_OK;
}

HRESULT WINAPI DirectDrawEnumerateA(LPDDENUMCALLBACKA a, LPVOID b) {
  HRESULT(WINAPI * real)(LPDDENUMCALLBACKA, LPVOID);
  HRESULT hr;
  enum_a_ctx e;
  hook_vulkan_surface_size();
  if (!load_real())
    return DDERR_GENERIC;
  real = (void *)GetProcAddress(g_real, "DirectDrawEnumerateA");
  ee_log("DirectDrawEnumerateA");
  if (g_enum_depth > 0) {
    ee_log("DirectDrawEnumerateA re-entry; answering with primary display");
    if (a)
      a(NULL, g_primary_desc, g_primary_name, b);
    return DD_OK;
  }
  e.cb = a;
  e.ctx = b;
  g_enum_seen = 0;
  g_enum_depth++;
  hr = real ? real(tramp_enum_a, &e) : DDERR_GENERIC;
  g_enum_depth--;
  if (a && g_enum_seen == 0) {
    ee_log("DirectDrawEnumerateA enumerated nothing (hr=0x%08lx); synthesising primary display",
           (unsigned long)hr);
    a(NULL, g_primary_desc, g_primary_name, b);
    hr = DD_OK;
  }
  ee_log("DirectDrawEnumerateA hr=0x%08lx adapters=%d", (unsigned long)hr, g_enum_seen);
  return hr;
}

HRESULT WINAPI DirectDrawEnumerateExA(LPDDENUMCALLBACKEXA a, LPVOID b, DWORD c) {
  HRESULT(WINAPI * real)(LPDDENUMCALLBACKEXA, LPVOID, DWORD);
  HRESULT hr;
  enum_ex_ctx e;
  hook_vulkan_surface_size();
  if (!load_real())
    return DDERR_GENERIC;
  real = (void *)GetProcAddress(g_real, "DirectDrawEnumerateExA");
  ee_log("DirectDrawEnumerateExA flags=0x%lx", (unsigned long)c);
  if (g_enum_depth > 0) {
    HRESULT(WINAPI * sysfn)(LPDDENUMCALLBACKEXA, LPVOID, DWORD) = NULL;
    if (load_sysdd())
      sysfn = (void *)GetProcAddress(g_sysdd, "DirectDrawEnumerateExA");
    if (sysfn) {
      ee_log("DirectDrawEnumerateExA re-entry -> builtin ddraw");
      return sysfn(a, b, c);
    }
    ee_log("DirectDrawEnumerateExA re-entry; answering with primary display");
    if (a)
      a(NULL, g_primary_desc, g_primary_name, b, NULL);
    return DD_OK;
  }
  e.cb = a;
  e.ctx = b;
  g_enum_seen = 0;
  g_enum_depth++;
  hr = real ? real(tramp_enum_ex, &e, c) : DDERR_GENERIC;
  g_enum_depth--;
  if (a && g_enum_seen == 0) {
    ee_log("DirectDrawEnumerateExA enumerated nothing (hr=0x%08lx); synthesising primary display",
           (unsigned long)hr);
    a(NULL, g_primary_desc, g_primary_name, b, NULL);
    hr = DD_OK;
  }
  ee_log("DirectDrawEnumerateExA hr=0x%08lx adapters=%d", (unsigned long)hr, g_enum_seen);
  return hr;
}

FWD(HRESULT, DirectDrawCreateClipper, (DWORD a, LPDIRECTDRAWCLIPPER *b, IUnknown *c), (a, b, c))
FWD(HRESULT, DirectDrawEnumerateW, (LPDDENUMCALLBACKW a, LPVOID b), (a, b))
FWD(HRESULT, DirectDrawEnumerateExW, (LPDDENUMCALLBACKEXW a, LPVOID b, DWORD c), (a, b, c))
FWD(HRESULT, DllCanUnloadNow, (void), ())
FWD(HRESULT, DllGetClassObject, (REFCLSID a, REFIID b, LPVOID *c), (a, b, c))

void WINAPI AcquireDDThreadLock(void) {
  void(WINAPI * real)(void);
  if (!load_real())
    return;
  real = (void *)GetProcAddress(g_real, "AcquireDDThreadLock");
  if (real)
    real();
}

void WINAPI ReleaseDDThreadLock(void) {
  void(WINAPI * real)(void);
  if (!load_real())
    return;
  real = (void *)GetProcAddress(g_real, "ReleaseDDThreadLock");
  if (real)
    real();
}

HRESULT WINAPI D3DParseUnknownCommand(LPVOID a, LPVOID *b) {
  HRESULT(WINAPI * real)(LPVOID, LPVOID *);
  if (!load_real())
    return E_FAIL;
  real = (void *)GetProcAddress(g_real, "D3DParseUnknownCommand");
  return real ? real(a, b) : E_FAIL;
}

HRESULT WINAPI CompleteCreateSysmemSurface(void *a) {
  HRESULT(WINAPI * real)(void *);
  if (!load_real())
    return E_FAIL;
  real = (void *)GetProcAddress(g_real, "CompleteCreateSysmemSurface");
  return real ? real(a) : E_FAIL;
}

HRESULT WINAPI DDInternalLock(void *a, void *b) {
  HRESULT(WINAPI * real)(void *, void *);
  if (!load_real())
    return E_FAIL;
  real = (void *)GetProcAddress(g_real, "DDInternalLock");
  return real ? real(a, b) : E_FAIL;
}

HRESULT WINAPI DDInternalUnlock(void *a) {
  HRESULT(WINAPI * real)(void *);
  if (!load_real())
    return E_FAIL;
  real = (void *)GetProcAddress(g_real, "DDInternalUnlock");
  return real ? real(a) : E_FAIL;
}
