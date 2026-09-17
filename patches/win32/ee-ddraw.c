/* Load dgVoodoo as ddraw_eeorig.dll and fake 800x600 display modes at process start. */
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

/* What we report when the wrapper says the card has no video memory. */
#define EE_FAKE_VIDMEM (256u * 1024u * 1024u)

static HMODULE g_real;
static FILE *g_log;
static int(WINAPI *orig_GetSystemMetrics)(int);
static void ee_log(const char *fmt, ...);

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
static volatile LONG g_n_surfaces, g_n_locks, g_n_frames, g_n_blts;

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
    return DD_OK;
  }
  ee_log("SetDisplayMode %lux%lux%lu refresh=%lu flags=0x%lx", (unsigned long)w, (unsigned long)h, (unsigned long)bpp,
         (unsigned long)refresh, (unsigned long)flags);
  hr = orig_SetDisplayMode7 ? orig_SetDisplayMode7(this, w, h, bpp, refresh, flags) : DD_OK;
  ee_log("SetDisplayMode hr=0x%08lx", (unsigned long)hr);
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

static DWORD WINAPI keep_foreground(void *arg) {
  int restores = 0;
  int ticks = 0;
  int moves = 0;
  (void)arg;
  for (;;) {
    HWND h = g_coop_hwnd;
    /* Ctrl+Alt+Q -- let me out: release any cursor clip, stop restoring the
     * window, and minimize so macOS can show something else.
     * Ctrl+Alt+X -- quit the game outright. */
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000)) {
      if (GetAsyncKeyState('Q') & 0x8000) {
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
      if (GetAsyncKeyState('X') & 0x8000) {
        ee_log("PANIC Ctrl+Alt+X: quitting");
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
     * stalled.  Print them for ten minutes, then stop. */
    if ((ticks % 40) == 20 && ticks / 40 < 300) {
      static LONG last_s, last_l, last_f;
      LONG cs = g_n_surfaces, cl = g_n_locks, cf = g_n_frames;
      ee_log("progress: surfaces=%ld(+%ld) locks=%ld(+%ld) frames=%ld(+%ld) blts=%ld", (long)cs,
             (long)(cs - last_s), (long)cl, (long)(cl - last_l), (long)cf, (long)(cf - last_f),
             (long)g_n_blts);
      last_s = cs;
      last_l = cl;
      last_f = cf;
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

static HRESULT STDMETHODCALLTYPE hook_SetCoop7(IDirectDraw7 *this, HWND hwnd, DWORD flags) {
  HRESULT hr;
  ee_log("SetCooperativeLevel hwnd=%p flags=0x%lx", (void *)hwnd, (unsigned long)flags);
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
    start_window_watchdog();
  }
  return hr;
}

static HRESULT STDMETHODCALLTYPE hook_CreateSurface7(IDirectDraw7 *this, DDSURFACEDESC2 *desc,
                                                     IDirectDrawSurface7 **surf, IUnknown *outer) {
  HRESULT hr;
  static unsigned n;
  /* The primary is the one that matters; a loading screen creates thousands of
   * 256x256 texture surfaces.  Always log a primary (DDSCAPS_PRIMARYSURFACE),
   * throttle the rest. */
  int loud = (desc && (desc->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE)) || hot_ok(&n);
  if (loud) {
    if (desc)
      ee_log("CreateSurface %lux%lu flags=0x%lx caps=0x%lx (dd=%p)", (unsigned long)desc->dwWidth,
             (unsigned long)desc->dwHeight, (unsigned long)desc->dwFlags, (unsigned long)desc->ddsCaps.dwCaps,
             (void *)this);
    else
      ee_log("CreateSurface desc=NULL");
  }
  hr = orig_CreateSurface7 ? orig_CreateSurface7(this, desc, surf, outer) : DDERR_GENERIC;
  if (SUCCEEDED(hr))
    g_n_surfaces++;
  if (loud || FAILED(hr))
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
    ee_log("  HW caps=0x%lx vidmem total=%lu free=%lu", (unsigned long)hw->dwCaps,
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
  return hr;
}

static HRESULT STDMETHODCALLTYPE hook_SurfLock(IDirectDrawSurface7 *this, LPRECT r, DDSURFACEDESC2 *sd, DWORD flags,
                                               HANDLE ev) {
  HRESULT hr;
  static unsigned n;
  int loud = hot_ok(&n);
  if (loud)
    ee_log("Surface::Lock flags=0x%lx", (unsigned long)flags);
  hr = orig_SurfLock ? orig_SurfLock(this, r, sd, flags, ev) : DDERR_GENERIC;
  g_n_locks++;
  if (loud)
    ee_log("Surface::Lock hr=0x%08lx", (unsigned long)hr);
  return hr;
}

static HRESULT STDMETHODCALLTYPE hook_SurfGetDC(IDirectDrawSurface7 *this, HDC *dc) {
  HRESULT hr;
  static unsigned n;
  int loud = hot_ok(&n);
  if (loud)
    ee_log("Surface::GetDC");
  hr = orig_SurfGetDC ? orig_SurfGetDC(this, dc) : DDERR_GENERIC;
  if (loud || FAILED(hr))
    ee_log("Surface::GetDC hr=0x%08lx", (unsigned long)hr);
  return hr;
}

/* The primary is FLIP|COMPLEX, so the game presents by calling Flip on it.
 * Everything renders (Clear, Blt, 82 Locks, 106 surfaces) yet the screen is
 * solid black at full-screen size -- so presentation is the missing link.
 * Slot 11 = Flip. */
static HRESULT(STDMETHODCALLTYPE *orig_SurfFlip)(IDirectDrawSurface7 *, IDirectDrawSurface7 *, DWORD);

static HRESULT STDMETHODCALLTYPE hook_SurfFlip(IDirectDrawSurface7 *this, IDirectDrawSurface7 *target, DWORD flags) {
  static int n;
  HRESULT hr;
  {
    static unsigned h;
    if (hot_ok(&h))
      ee_log("Surface::Flip ENTER this=%p target=%p flags=0x%lx", (void *)this, (void *)target,
             (unsigned long)flags);
  }
  hr = orig_SurfFlip ? orig_SurfFlip(this, target, flags) : DDERR_GENERIC;
  if (n++ < 12 || FAILED(hr) || (n % 20000) == 0)
    ee_log("Surface::Flip this=%p target=%p flags=0x%lx hr=0x%08lx%s (#%d)", (void *)this, (void *)target,
           (unsigned long)flags, (unsigned long)hr, hr == DDERR_SURFACELOST ? " DDERR_SURFACELOST" : "", n);
  if (FAILED(hr) && n <= 20)
    log_window_state("flip-failed");
  return hr;
}

static HRESULT STDMETHODCALLTYPE hook_SurfBlt(IDirectDrawSurface7 *this, LPRECT dst, IDirectDrawSurface7 *src, LPRECT sr,
                                              DWORD flags, DDBLTFX *fx) {
  HRESULT hr;
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
  hr = orig_SurfBlt ? orig_SurfBlt(this, dst, src, sr, flags, fx) : DDERR_GENERIC;
  g_n_blts++;
  {
    static unsigned n;
    if (hot_ok(&n) || FAILED(hr))
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
    ee_log("  device \"%s\" / \"%s\" guid=%s devcaps=0x%lx maxtex=%lu", name ? name : "?", desc ? desc : "?",
           device_name(&dd->deviceGUID), (unsigned long)dd->dwDevCaps, (unsigned long)dd->wMaxSimultaneousTextures);
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
  ee_log("IDirect3D7::CreateDevice %s this=%p surface=%p", device_name(rclsid), (void *)this, (void *)surf);
  hr = orig_D3DCreateDevice ? orig_D3DCreateDevice(this, rclsid, surf, dev) : DDERR_GENERIC;
  ee_log("IDirect3D7::CreateDevice hr=0x%08lx dev=%p", (unsigned long)hr, (void *)(dev ? *dev : NULL));
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

static HRESULT STDMETHODCALLTYPE hook_DevGetCaps(IDirect3DDevice7 *this, D3DDEVICEDESC7 *d) {
  HRESULT hr = orig_DevGetCaps ? orig_DevGetCaps(this, d) : DDERR_GENERIC;
  ee_log("Device7::GetCaps hr=0x%08lx", (unsigned long)hr);
  return hr;
}

static HRESULT STDMETHODCALLTYPE hook_DevBeginScene(IDirect3DDevice7 *this) {
  HRESULT hr;
  static unsigned n;
  int loud = hot_ok(&n);
  if (loud)
    ee_log("Device7::BeginScene ENTER this=%p", (void *)this);
  hr = orig_DevBeginScene ? orig_DevBeginScene(this) : DDERR_GENERIC;
  g_n_frames++;
  if (SUCCEEDED(hr) && !g_rendering) {
    g_rendering = 1;
    ee_log("first BeginScene succeeded -- window watchdog now hands the window back to the player");
  }
  if (loud || FAILED(hr))
    ee_log("Device7::BeginScene hr=0x%08lx", (unsigned long)hr);
  return hr;
}

static HRESULT STDMETHODCALLTYPE hook_DevEndScene(IDirect3DDevice7 *this) {
  HRESULT hr;
  static unsigned n;
  int loud = hot_ok(&n);
  if (loud)
    ee_log("Device7::EndScene ENTER this=%p", (void *)this);
  hr = orig_DevEndScene ? orig_DevEndScene(this) : DDERR_GENERIC;
  if (loud || FAILED(hr))
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
  int loud = hot_ok(&n);
  if (loud)
    ee_log("Device7::Clear ENTER flags=0x%lx count=%lu", (unsigned long)flags, (unsigned long)count);
  hr = orig_DevClear ? orig_DevClear(this, count, rects, flags, color, z, stencil) : DDERR_GENERIC;
  if (loud || FAILED(hr))
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
static HRESULT(STDMETHODCALLTYPE *orig_DevGetRT)(IDirect3DDevice7 *, IDirectDrawSurface7 **);
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
  ee_log("wrapped IDirect3DDevice7 vtable %p", obj);
  log_null_slots("IDirect3DDevice7", vt, 49);
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
  int loud = hot_ok(&n);
  if (loud)
    ee_log("Surface::Unlock ENTER this=%p", (void *)this);
  hr = orig_SurfUnlock ? orig_SurfUnlock(this, r) : DDERR_GENERIC;
  if (loud || FAILED(hr))
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
  if (!g_log) {
    g_log = fopen("ee-ddraw.log", "w");
    if (g_log)
      setvbuf(g_log, NULL, _IONBF, 0);
  }
  fputs("ee-ddraw: ", stderr);
  if (g_log)
    fputs("ee-ddraw: ", g_log);
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
  if (reason == DLL_PROCESS_ATTACH)
    load_real();
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
  if (!load_real())
    return DDERR_GENERIC;
  real = (void *)GetProcAddress(g_real, "DirectDrawCreate");
  ee_log("DirectDrawCreate");
  hr = real ? real(a, b, c) : DDERR_GENERIC;
  ee_log("DirectDrawCreate hr=0x%08lx", (unsigned long)hr);
  return hr;
}

static int g_create_depth;

HRESULT WINAPI DirectDrawCreateEx(GUID *a, LPVOID *b, REFIID c, IUnknown *d) {
  HRESULT(WINAPI * real)(GUID *, LPVOID *, REFIID, IUnknown *);
  HRESULT hr;
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
