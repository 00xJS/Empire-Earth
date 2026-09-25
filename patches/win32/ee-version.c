/* version.dll proxy: DllMain runs before WinMain.
 * DX7HRTnLDisplay.dll is LoadLibrary'd after the splash. Poll for it and
 * IAT-patch CreateWindowExA there only — never hook LoadLibrary (loader lock)
 * and never patch Wine builtins.
 */
#include <windows.h>
#include <tlhelp32.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "ee-ssemath.h"

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

static HMODULE g_real;
static HMODULE g_self;
static FILE *g_log;

static HWND(WINAPI *orig_CreateWindowExA)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU,
                                          HINSTANCE, LPVOID);
static BOOL(WINAPI *orig_AdjustWindowRect)(LPRECT, DWORD, BOOL);
static int(WINAPI *orig_GetSystemMetrics)(int);
static HMODULE(WINAPI *orig_LoadLibraryA)(LPCSTR);
static HMODULE(WINAPI *orig_LoadLibraryExA)(LPCSTR, HANDLE, DWORD);

static HMODULE WINAPI hook_LoadLibraryA(LPCSTR name);
static void tame_splash(HWND hwnd, const char *cls);
static HMODULE WINAPI hook_LoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags);

static void ee_log(const char *fmt, ...) {
  va_list ap;
  if (!g_log) {
    /* Appended across launches, so a crash's record survives the next start;
     * past 4 MB it moves to ee-version.old.log instead of growing for ever. */
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExA("ee-version.log", GetFileExInfoStandard, &fa) &&
        (fa.nFileSizeHigh || fa.nFileSizeLow > 4u * 1024 * 1024))
      MoveFileExA("ee-version.log", "ee-version.old.log", MOVEFILE_REPLACE_EXISTING);
    g_log = fopen("ee-version.log", "a");
    if (g_log)
      setvbuf(g_log, NULL, _IONBF, 0);
  }
  fputs("ee-version: ", stderr);
  if (g_log)
    fputs("ee-version: ", g_log);
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

static int is_splash_size(int w, int h) { return w >= 630 && w <= 650 && h >= 250 && h <= 270; }

static int is_default_size(int v) { return v == CW_USEDEFAULT || v == (int)0x80000000; }

static int is_toplevel(HWND parent, DWORD style) {
  if (style & WS_CHILD)
    return 0;
  if (parent && parent != HWND_DESKTOP)
    return 0;
  if (parent == HWND_MESSAGE)
    return 0;
  return 1;
}

static int is_tiny_size(int w, int h) {
  if (is_default_size(w) || is_default_size(h))
    return 0;
  if (is_splash_size(w, h))
    return 0;
  if (w <= 0 || h <= 0)
    return 0;
  return w < 320 || h < 240;
}

/* ---- the screen size these shims report --------------------------------
 * All of this used to be a hard-coded 800x600, from the era when the game's
 * window came up degenerate and any sane constant was an improvement.  With the
 * Wine virtual desktop working, that constant is now a lie that tells Empire
 * Earth its screen is smaller than it is.  Report the real desktop instead --
 * scripts/launch.sh exports it as EE_SCREEN_SIZE from VIRTUAL_DESKTOP_SIZE. */
static void ee_screen(int *w, int *h) {
  static int cw, ch;
  if (!cw) {
    char b[32];
    int a = 0, c = 0;
    cw = 1440;
    ch = 933;
    if (GetEnvironmentVariableA("EE_SCREEN_SIZE", b, sizeof b) > 0 && sscanf(b, "%dx%d", &a, &c) == 2 &&
        a >= 320 && c >= 240) {
      cw = a;
      ch = c;
    }
    /* Deliberately no ee_log here.  ee_screen() is reached from
     * GetSystemMetrics, which the game calls from inside
     * DirectDrawEnumerateExA -- and logging from that path risks the loader
     * lock this project has already been bitten by.  The size is echoed by the
     * first EnumDisplaySettingsA line instead. */
  }
  *w = cw;
  *h = ch;
}

static void fill_mode(DEVMODEA *dm) {
  int sw, sh;
  if (!dm)
    return;
  if (!dm->dmSize)
    dm->dmSize = sizeof(DEVMODEA);
  dm->dmFields |= DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
  ee_screen(&sw, &sh);
  dm->dmPelsWidth = (DWORD)sw;
  dm->dmPelsHeight = (DWORD)sh;
  dm->dmBitsPerPel = 32;
  dm->dmDisplayFrequency = 60;
}

static BOOL WINAPI hook_EnumDisplaySettingsA(const char *dev, DWORD mode, DEVMODEA *dm) {
  (void)dev;
  if (!dm)
    return FALSE;
  if (mode == ENUM_CURRENT_SETTINGS || mode == ENUM_REGISTRY_SETTINGS || mode <= 8) {
    fill_mode(dm);
    ee_log("EnumDisplaySettingsA %lu -> %lux%lu", (unsigned long)mode, (unsigned long)dm->dmPelsWidth,
           (unsigned long)dm->dmPelsHeight);
    return TRUE;
  }
  return FALSE;
}

static BOOL WINAPI hook_EnumDisplaySettingsExA(const char *dev, DWORD mode, DEVMODEA *dm, DWORD flags) {
  (void)flags;
  return hook_EnumDisplaySettingsA(dev, mode, dm);
}

static LONG WINAPI hook_ChangeDisplaySettingsA(DEVMODEA *dm, DWORD flags) {
  (void)dm;
  (void)flags;
  ee_log("ChangeDisplaySettingsA SUCCESS");
  return DISP_CHANGE_SUCCESSFUL;
}

static int WINAPI hook_GetSystemMetrics(int idx) {
  int sw, sh;
  ee_screen(&sw, &sh);
  if (idx == SM_CXSCREEN || idx == SM_CXFULLSCREEN || idx == SM_CXVIRTUALSCREEN)
    return sw;
  if (idx == SM_CYSCREEN || idx == SM_CYFULLSCREEN || idx == SM_CYVIRTUALSCREEN)
    return sh;
  return orig_GetSystemMetrics ? orig_GetSystemMetrics(idx) : sw;
}

static BOOL WINAPI hook_AdjustWindowRect(LPRECT rc, DWORD style, BOOL menu) {
  if (rc && is_tiny_size(rc->right - rc->left, rc->bottom - rc->top) && !(style & WS_CHILD)) {
    int sw, sh;
    ee_screen(&sw, &sh);
    ee_log("AdjustWindowRect %dx%d -> %dx%d", (int)(rc->right - rc->left), (int)(rc->bottom - rc->top), sw, sh);
    rc->right = rc->left + sw;
    rc->bottom = rc->top + sh;
  }
  return orig_AdjustWindowRect ? orig_AdjustWindowRect(rc, style, menu) : TRUE;
}

static HWND WINAPI hook_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style, int x, int y, int w,
                                        int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
  char cbuf[64] = {0};
  char tbuf[64] = {0};
  int test_wnd = 0;
  if (cls && !IS_INTRESOURCE(cls))
    lstrcpynA(cbuf, cls, sizeof cbuf);
  else if (cls)
    wsprintfA(cbuf, "#%u", (unsigned)(ULONG_PTR)cls);
  if (title)
    lstrcpynA(tbuf, title, sizeof tbuf);
  test_wnd = strstr(cbuf, "Test") != NULL;
  /* Rasterizer capability probes are 16x16 TestWindowClass. Enlarging them
   * leaves a black 800x600 window on screen and the splash never hides. */
  if (!test_wnd && is_toplevel(parent, style) && is_tiny_size(w, h)) {
    int sw, sh;
    ee_screen(&sw, &sh);
    ee_log("CreateWindowExA class='%s' title='%s' %dx%d style=0x%lx -> %dx%d", cbuf, tbuf, w, h,
           (unsigned long)style, sw, sh);
    w = sw;
    h = sh;
  } else {
    ee_log("CreateWindowExA class='%s' title='%s' %dx%d parent=%p style=0x%lx", cbuf, tbuf, w, h,
           (void *)parent, (unsigned long)style);
  }
  {
    HWND hwnd = orig_CreateWindowExA(ex, cls, title, style, x, y, w, h, parent, menu, inst, param);
    tame_splash(hwnd, cbuf);
    return hwnd;
  }
}

static void *rva_ptr(HMODULE mod, DWORD rva) { return (char *)mod + rva; }

/* The "Loading Game Window" splash never validates its update region: its own
 * WM_PAINT handler returns 0 without BeginPaint/EndPaint, so Wine re-delivers
 * WM_PAINT forever. A +msg trace shows 21,198 WM_PAINT to that one window,
 * monopolising the message queue while the loader thread tries to make progress.
 * Subclass it: let the game paint as usual, then validate so the storm stops.
 * EE_NO_SPLASH_FIX=1 disables this. */
static WNDPROC g_splash_orig;

static LRESULT CALLBACK ee_splash_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  LRESULT r;
  if (!g_splash_orig)
    return DefWindowProcA(hwnd, msg, wp, lp);
  r = CallWindowProcA(g_splash_orig, hwnd, msg, wp, lp);
  if (msg == WM_PAINT) {
    static int n;
    ValidateRect(hwnd, NULL);
    if (n++ < 3)
      ee_log("splash WM_PAINT validated (storm suppressed)");
  }
  return r;
}

static void tame_splash(HWND hwnd, const char *cls) {
  char buf[64];
  unsigned i;
  if (!hwnd || !cls || g_splash_orig)
    return;
  for (i = 0; cls[i] && i + 1 < sizeof buf; i++) {
    char c = cls[i];
    if (c >= 'A' && c <= 'Z')
      c = (char)(c - 'A' + 'a');
    buf[i] = c;
  }
  buf[i] = 0;
  if (!strstr(buf, "loading game window"))
    return;
  {
    char env[8];
    if (GetEnvironmentVariableA("EE_NO_SPLASH_FIX", env, sizeof env) > 0 && env[0] == '1')
      return;
  }
  g_splash_orig = (WNDPROC)(LONG_PTR)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)ee_splash_proc);
  ee_log("subclassed splash hwnd=%p to stop the WM_PAINT storm", (void *)hwnd);
}

static int patch_iat(HMODULE mod, const char *dllwant, const char *fn, void *hook) {
  IMAGE_DOS_HEADER *dos;
  IMAGE_NT_HEADERS *nt;
  IMAGE_IMPORT_DESCRIPTOR *imp;
  DWORD imp_rva;
  if (!mod || mod == g_self)
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
      if (th->u1.Function == (ULONG_PTR)hook)
        return 1;
      if (!VirtualProtect(&th->u1.Function, sizeof(th->u1.Function), PAGE_READWRITE, &old))
        return 0;
      th->u1.Function = (ULONG_PTR)hook;
      VirtualProtect(&th->u1.Function, sizeof(th->u1.Function), old, &old);
      ee_log("IAT %p %s!%s", (void *)mod, dll, fn);
      return 1;
    }
  }
  return 0;
}

/* ---- abort() forensics ----------------------------------------------------
 * Empire Earth terminates itself with the CRT's abort() (Wine logs
 * "raise (22)") from inside its fullscreen resize handling, after making no
 * DirectDraw/Direct3D call at all. Nothing in the log says why.
 *
 * This DLL and the game both import the same msvcrt.dll, so the CRT's signal
 * table is shared: signal(SIGABRT, ...) installed here catches the game's abort,
 * including one raised inside msvcrt itself. On entry we walk the EBP chain and
 * resolve each return address to module+offset, which names the code that made
 * the decision. Logging is unbuffered, so it survives the abort.
 */
static int addr_readable(const void *p, size_t n) {
  MEMORY_BASIC_INFORMATION mbi;
  if (!p)
    return 0;
  if (!VirtualQuery(p, &mbi, sizeof mbi))
    return 0;
  if (mbi.State != MEM_COMMIT)
    return 0;
  if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
    return 0;
  return (size_t)((const char *)mbi.BaseAddress + mbi.RegionSize - (const char *)p) >= n;
}

static void log_frame(unsigned idx, void *ret) {
  HMODULE mod = NULL;
  char path[MAX_PATH];
  const char *base;
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         (LPCSTR)ret, &mod) &&
      mod && GetModuleFileNameA(mod, path, sizeof path)) {
    base = strrchr(path, '\\');
    base = base ? base + 1 : path;
    ee_log("  #%u %p  %s+0x%lx", idx, ret, base, (unsigned long)((char *)ret - (char *)mod));
  } else {
    ee_log("  #%u %p  <unknown module>", idx, ret);
  }
}

static void log_stack(const char *why) {
  void **frame;
  unsigned i;
  __asm__ volatile("mov %%ebp, %0" : "=r"(frame));
  ee_log("=== %s === stack follows (return addresses)", why);
  for (i = 0; i < 24 && frame; i++) {
    void *ret;
    void **next;
    if (!addr_readable(frame, 2 * sizeof(void *)))
      break;
    ret = frame[1];
    next = (void **)frame[0];
    if (!ret)
      break;
    log_frame(i, ret);
    if (next <= frame)
      break;
    frame = next;
  }
  ee_log("=== %s === end of stack", why);
  fflush(NULL);
}

static void ee_abort_handler(int sig) {
  ee_log("!!! CRT signal %d (SIGABRT=%d) -- the game is terminating itself", sig, SIGABRT);
  log_stack("abort");
  /* Do not return: returning from a SIGABRT handler is undefined. Let the
   * default action run so behaviour is unchanged apart from the logging. */
  signal(SIGABRT, SIG_DFL);
  raise(SIGABRT);
}

/* Low-Level Engine.dll's assertion helper formats
 *   "%s (%d) : Verification failure (%s)"   (file, line, expression)
 * into a 1KB buffer and then calls this import with the buffer as its only
 * argument (the call site cleans 0x18 bytes: 5 args for the sprintf plus one
 * for this). Declared __cdecl with one parameter so it is also safe when
 * something really does call abort() with no arguments -- the caller cleans up,
 * and we validate the pointer before touching it. */
static int printable_str(const char *s) {
  unsigned i;
  if (!addr_readable(s, 1))
    return 0;
  for (i = 0; i < 512; i++) {
    if (!addr_readable(s + i, 1))
      return 0;
    if (s[i] == 0)
      return i > 3;
    if ((unsigned char)s[i] < 9 || (unsigned char)s[i] > 126)
      return 0;
  }
  return 0;
}

static void __cdecl hook_abort(const char *msg) {
  /* Entered by a normal CALL through the IAT, so our own return address is the
   * caller -- far more reliable than walking EBP through optimised C++. */
  void *ra = __builtin_return_address(0);
  ee_log("!!! abort()/fatal import called -- caller:");
  log_frame(0, ra);
  if (printable_str(msg))
    ee_log(">>> FATAL MESSAGE: %s", msg);
  else
    ee_log(">>> (no printable message argument)");
  log_stack("abort-iat");
  signal(SIGABRT, SIG_DFL);
  abort();
}

static int __cdecl hook_raise(int sig) {
  int (*orig)(int);
  void *ra = __builtin_return_address(0);
  ee_log("!!! raise(%d) called via IAT -- caller:", sig);
  log_frame(0, ra);
  log_stack("raise-iat");
  orig = (int (*)(int))GetProcAddress(GetModuleHandleA("msvcrt.dll"), "raise");
  return orig ? orig(sig) : 0;
}

/* abort()/raise() can come from any of the game's modules -- the observed
 * raise(22) is not from the exe or the DX7HR rasterizers, so patch the CRT
 * imports of every module we see load. patch_iat is a no-op when the import is
 * absent, so this is cheap and harmless. */
/* ---- code detour on msvcrt!raise -----------------------------------------
 * The game does not import abort/raise by name, so IAT hooking cannot see the
 * SIGABRT it kills itself with. Patch the first 5 bytes of the exported
 * msvcrt!raise with a JMP to us instead. Entered via JMP, our return address is
 * the game's call site -- exactly the address we need.
 * No trampoline / instruction decoding: we restore the original bytes before
 * calling through, which is safe because abort is terminal anyway.
 */
static unsigned char g_raise_orig[5];
static void *g_raise_addr;
static int g_raise_patched;
static int __cdecl ee_raise_detour(int sig);

static void unpatch_raise(void) {
  DWORD old;
  if (!g_raise_patched || !g_raise_addr)
    return;
  if (VirtualProtect(g_raise_addr, 5, PAGE_EXECUTE_READWRITE, &old)) {
    memcpy(g_raise_addr, g_raise_orig, 5);
    VirtualProtect(g_raise_addr, 5, old, &old);
  }
  g_raise_patched = 0;
}

static void install_raise_detour(void) {
  HMODULE m = GetModuleHandleA("msvcrt.dll");
  unsigned char *fn = m ? (unsigned char *)GetProcAddress(m, "raise") : NULL;
  DWORD old;
  int rel;
  if (!fn) {
    ee_log("raise detour: msvcrt!raise not found");
    return;
  }
  g_raise_addr = fn;
  memcpy(g_raise_orig, fn, 5);
  if (!VirtualProtect(fn, 5, PAGE_EXECUTE_READWRITE, &old)) {
    ee_log("raise detour: VirtualProtect failed %lu", (unsigned long)GetLastError());
    return;
  }
  rel = (int)((char *)ee_raise_detour - ((char *)fn + 5));
  fn[0] = 0xE9;
  memcpy(fn + 1, &rel, 4);
  VirtualProtect(fn, 5, old, &old);
  g_raise_patched = 1;
  ee_log("detoured msvcrt!raise at %p -> %p", (void *)fn, (void *)ee_raise_detour);
}

static int __cdecl ee_raise_detour(int sig) {
  void *ra = __builtin_return_address(0);
  ee_log("!!! msvcrt!raise(%d) -- the game is terminating itself", sig);
  ee_log("call site:");
  log_frame(0, ra);
  log_stack("raise-detour");
  unpatch_raise();
  {
    int(__cdecl * real)(int) = (int(__cdecl *)(int))g_raise_addr;
    return real ? real(sig) : 0;
  }
}

/* DO NOT call this from the LoadLibrary hooks. Walking a module's import table
 * while it is still being loaded runs under the loader lock, and doing it for
 * EVERY module made the game deadlock at 0% CPU roughly four runs in five -- the
 * "flaky early exit" that dominated 16 Sep. The abort forensics it fed have
 * already done their job (Empire Earth.cpp:2506 is found and fixed), so this is
 * now only wired into the two explicit, rare patch points below. */
static void patch_crt(HMODULE mod) {
  if (!mod)
    return;
  patch_iat(mod, "msvcrt.dll", "abort", (void *)hook_abort);
  patch_iat(mod, "msvcrt.dll", "raise", (void *)hook_raise);
  patch_iat(mod, "MSVCP60.dll", "abort", (void *)hook_abort);
  patch_iat(mod, "MSVCP60.dll", "raise", (void *)hook_raise);
}

static void patch_rasterizer(HMODULE mod) {
  if (!mod)
    return;
  patch_iat(mod, "user32.dll", "CreateWindowExA", (void *)hook_CreateWindowExA);
  patch_iat(mod, "user32.dll", "AdjustWindowRect", (void *)hook_AdjustWindowRect);
  patch_iat(mod, "msvcrt.dll", "abort", (void *)hook_abort);
  patch_iat(mod, "msvcrt.dll", "raise", (void *)hook_raise);
}

/* Low-Level Engine.dll holds the Verify()/assertion helper that formats
 * "%s (%d) : Verification failure (%s)" and calls the fatal import. Patch its
 * CRT imports by name so the message is captured -- targeted, unlike the
 * every-module patching from the LoadLibrary hooks that deadlocked the loader. */
static int name_has_engine(const char *name) {
  char buf[MAX_PATH];
  unsigned i;
  if (!name)
    return 0;
  for (i = 0; name[i] && i + 1 < sizeof buf; i++) {
    char c = name[i];
    if (c >= 'A' && c <= 'Z')
      c = (char)(c - 'A' + 'a');
    buf[i] = c;
  }
  buf[i] = 0;
  return strstr(buf, "low-level engine") != NULL;
}

static int name_has_dx7(const char *name) {
  char buf[MAX_PATH];
  unsigned i;
  if (!name)
    return 0;
  for (i = 0; name[i] && i + 1 < sizeof buf; i++) {
    char c = name[i];
    if (c >= 'A' && c <= 'Z')
      c = (char)(c - 'A' + 'a');
    buf[i] = c;
  }
  buf[i] = 0;
  return strstr(buf, "dx7hr") != NULL;
}

static HMODULE WINAPI hook_LoadLibraryA(LPCSTR name) {
  HMODULE mod = orig_LoadLibraryA(name);
  if (name)
    ee_log("LoadLibraryA %s -> %p", name, (void *)mod);
  if (mod && name_has_dx7(name))
    patch_rasterizer(mod);
  if (mod && name_has_engine(name))
    patch_crt(mod);
  return mod;
}

static HMODULE WINAPI hook_LoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags) {
  HMODULE mod = orig_LoadLibraryExA(name, file, flags);
  if (name)
    ee_log("LoadLibraryExA %s flags=0x%lx -> %p", name, (unsigned long)flags, (void *)mod);
  if (mod && name_has_dx7(name))
    patch_rasterizer(mod);
  if (mod && name_has_engine(name))
    patch_crt(mod);
  return mod;
}

static LPTOP_LEVEL_EXCEPTION_FILTER WINAPI hook_SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER f);
static void __attribute__((thiscall)) hook_BuildPointList(void *self, void *vec, long step);
#define PL_EXPORT_NAME "?BuildPointList@U2DSparseArrayPointContainer@@QAEXAAV?$vector@V?$U2DPoint@J@@V?$allocator@V?$U2DPoint@J@@@std@@@std@@J@Z"

static void patch_exe_modes(HMODULE mod) {
  if (!mod)
    return;
  patch_iat(mod, "kernel32.dll", "SetUnhandledExceptionFilter", (void *)hook_SetUnhandledExceptionFilter);
  patch_iat(mod, "low-level engine.dll", PL_EXPORT_NAME, (void *)hook_BuildPointList);
  patch_iat(mod, "user32.dll", "CreateWindowExA", (void *)hook_CreateWindowExA);
  patch_iat(mod, "user32.dll", "EnumDisplaySettingsA", (void *)hook_EnumDisplaySettingsA);
  patch_iat(mod, "user32.dll", "EnumDisplaySettingsExA", (void *)hook_EnumDisplaySettingsExA);
  patch_iat(mod, "user32.dll", "ChangeDisplaySettingsA", (void *)hook_ChangeDisplaySettingsA);
  patch_iat(mod, "user32.dll", "GetSystemMetrics", (void *)hook_GetSystemMetrics);
  patch_iat(mod, "msvcrt.dll", "abort", (void *)hook_abort);
  patch_iat(mod, "msvcrt.dll", "raise", (void *)hook_raise);
  patch_iat(mod, "MSVCP60.dll", "abort", (void *)hook_abort);
}

static void resolve_orig(void) {
  HMODULE user = GetModuleHandleA("user32.dll");
  HMODULE kern = GetModuleHandleA("kernel32.dll");
  orig_CreateWindowExA = (void *)GetProcAddress(user, "CreateWindowExA");
  orig_AdjustWindowRect = (void *)GetProcAddress(user, "AdjustWindowRect");
  orig_GetSystemMetrics = (void *)GetProcAddress(user, "GetSystemMetrics");
  orig_LoadLibraryA = (void *)GetProcAddress(kern, "LoadLibraryA");
  orig_LoadLibraryExA = (void *)GetProcAddress(kern, "LoadLibraryExA");
}

static void install_mode_hooks(void) {
  static int once;
  HMODULE engine;
  if (once)
    return;
  once = 1;
  resolve_orig();
  patch_exe_modes(GetModuleHandleA(NULL));
  engine = GetModuleHandleA("Low-Level Engine.dll");
  if (engine) {
    patch_iat(engine, "kernel32.dll", "LoadLibraryA", (void *)hook_LoadLibraryA);
    patch_iat(engine, "kernel32.dll", "LoadLibraryExA", (void *)hook_LoadLibraryExA);
  }
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
  strcpy(slash + 1, "version_eeorig.dll");
  g_real = LoadLibraryA(path);
  if (!g_real) {
    ee_log("LoadLibrary %s failed %lu", path, (unsigned long)GetLastError());
    return 0;
  }
  ee_log("wrapping %s", path);
  install_mode_hooks();
  return 1;
}

/* ---- vectored exception handler ------------------------------------------
 * About half of all d7vk launches die with
 *     wine: Unhandled page fault on read access to 00004DC9 at address 7BF2123D
 * somewhere inside DirectDrawEnumerateExA, and Wine's own message gives only a
 * raw address -- no module, no offset, no stack.  A first-chance VEH runs
 * before Wine's debugger and can resolve all three, turning the intermittent
 * crash into a one-line answer about which DLL is at fault. */
static void log_ctx_stack(CONTEXT *c) {
  void **frame = (void **)(uintptr_t)c->Ebp;
  unsigned i;
  ee_log("  EAX=%08lx EBX=%08lx ECX=%08lx EDX=%08lx ESI=%08lx EDI=%08lx", (unsigned long)c->Eax,
         (unsigned long)c->Ebx, (unsigned long)c->Ecx, (unsigned long)c->Edx, (unsigned long)c->Esi,
         (unsigned long)c->Edi);
  ee_log("  ESP=%08lx EBP=%08lx EIP=%08lx", (unsigned long)c->Esp, (unsigned long)c->Ebp,
         (unsigned long)c->Eip);
  for (i = 0; i < 24 && frame; i++) {
    void *ret;
    void **next;
    if (!addr_readable(frame, 2 * sizeof(void *)))
      break;
    ret = frame[1];
    next = (void **)frame[0];
    if (!ret)
      break;
    log_frame(i, ret);
    if (next <= frame)
      break;
    frame = next;
  }
}

/* GetModuleHandleEx(FROM_ADDRESS) came back empty for the faulting address, so
 * resolve it the hard way: VirtualQuery for the mapping, then walk the module
 * list looking for the one whose image range contains it. */
static int g_dumped_modules;

static void describe_addr(const char *what, void *p) {
  MEMORY_BASIC_INFORMATION mbi;
  MODULEENTRY32 me;
  HANDLE snap;
  int found = 0;
  if (VirtualQuery(p, &mbi, sizeof mbi))
    ee_log("    %s %p: alloc=%p base=%p size=%lu state=0x%lx protect=0x%lx type=0x%lx", what, p,
           mbi.AllocationBase, mbi.BaseAddress, (unsigned long)mbi.RegionSize, (unsigned long)mbi.State,
           (unsigned long)mbi.Protect, (unsigned long)mbi.Type);
  else
    ee_log("    %s %p: VirtualQuery failed", what, p);
  /* MEM_IMAGE but absent from the PEB loader list: read the PE's own export
   * directory name straight out of the mapping.  That is how a module that
   * GetModuleHandleEx and toolhelp both deny still gets identified. */
  if (mbi.Type == MEM_IMAGE && mbi.AllocationBase && addr_readable(mbi.AllocationBase, 0x400)) {
    const IMAGE_DOS_HEADER *dh = (const IMAGE_DOS_HEADER *)mbi.AllocationBase;
    if (dh->e_magic == IMAGE_DOS_SIGNATURE) {
      const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)((const char *)dh + dh->e_lfanew);
      if (addr_readable((void *)nt, sizeof *nt) && nt->Signature == IMAGE_NT_SIGNATURE) {
        DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
        ee_log("    %s image at %p: SizeOfImage=%lu entry=+0x%lx exportRVA=0x%lx", what, mbi.AllocationBase,
               (unsigned long)nt->OptionalHeader.SizeOfImage,
               (unsigned long)nt->OptionalHeader.AddressOfEntryPoint, (unsigned long)rva);
        if (rva) {
          const IMAGE_EXPORT_DIRECTORY *ed = (const IMAGE_EXPORT_DIRECTORY *)((const char *)dh + rva);
          if (addr_readable((void *)ed, sizeof *ed) && ed->Name) {
            const char *nm = (const char *)dh + ed->Name;
            if (addr_readable((void *)nm, 4))
              ee_log("    %s image at %p is '%s'", what, mbi.AllocationBase, nm);
          }
        }
      }
    }
  }
  snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, 0);
  if (snap == INVALID_HANDLE_VALUE) {
    ee_log("    (toolhelp snapshot failed %lu)", (unsigned long)GetLastError());
    return;
  }
  me.dwSize = sizeof me;
  if (Module32First(snap, &me)) {
    do {
      if ((char *)p >= (char *)me.modBaseAddr && (char *)p < (char *)me.modBaseAddr + me.modBaseSize) {
        ee_log("    %s %p is %s+0x%lx (base %p size %lu)", what, p, me.szModule,
               (unsigned long)((char *)p - (char *)me.modBaseAddr), me.modBaseAddr,
               (unsigned long)me.modBaseSize);
        found = 1;
      }
    } while (Module32Next(snap, &me));
  }
  if (!found || !g_dumped_modules) {
    g_dumped_modules = 1;
    ee_log("    %s %p%s -- full module list follows:", what, p, found ? "" : " is in NO loaded module");
    me.dwSize = sizeof me;
    if (Module32First(snap, &me))
      do {
        ee_log("      %p +%-8lu %s", me.modBaseAddr, (unsigned long)me.modBaseSize, me.szModule);
      } while (Module32Next(snap, &me));
  }
  CloseHandle(snap);
}

/* EBP is not a frame pointer in optimised code (the crash context had EBP below
 * ESP), so scan the raw stack for values that land inside a loaded module. */
static void scan_stack(DWORD esp) {
  DWORD *p = (DWORD *)(uintptr_t)esp;
  unsigned i, shown = 0;
  for (i = 0; i < 256 && shown < 16; i++) {
    HMODULE mod = NULL;
    char path[MAX_PATH];
    void *v;
    if (!addr_readable(p + i, sizeof(DWORD)))
      break;
    v = (void *)(uintptr_t)p[i];
    if ((uintptr_t)v < 0x10000)
      continue;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)v, &mod) &&
        mod && GetModuleFileNameA(mod, path, sizeof path)) {
      const char *base = strrchr(path, '\\');
      base = base ? base + 1 : path;
      ee_log("    stack[%u] %p  %s+0x%lx", i, v, base, (unsigned long)((char *)v - (char *)mod));
      shown++;
    }
  }
}

static LONG CALLBACK ee_veh(EXCEPTION_POINTERS *ep) {
  static int n;
  DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
  /* Ignore the noise: C++ EH, RPC, breakpoints, and anything an SEH frame will
   * swallow anyway.  Access violations and illegal instructions are the ones
   * that actually kill this process. */
  if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
      code != EXCEPTION_PRIV_INSTRUCTION && code != EXCEPTION_IN_PAGE_ERROR)
    return EXCEPTION_CONTINUE_SEARCH;
  /* ntdll's own try/except machinery raises benign first-chance access
   * violations constantly; they swamped the first capture.  Skip them unless
   * nothing else has been logged yet (the first one also dumps the module map,
   * which is what actually identifies the mystery 7BF2xxxx faulting PC). */
  {
    HMODULE m = NULL;
    char mp[MAX_PATH];
    if (n > 0 &&
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)ep->ExceptionRecord->ExceptionAddress, &m) &&
        m && GetModuleFileNameA(m, mp, sizeof mp) && strstr(mp, "ntdll"))
      return EXCEPTION_CONTINUE_SEARCH;
  }
  if (n++ > 24)
    return EXCEPTION_CONTINUE_SEARCH;
  ee_log("!!! exception 0x%08lx at %p (first chance #%d)", (unsigned long)code,
         (void *)ep->ExceptionRecord->ExceptionAddress, n);
  log_frame(0, ep->ExceptionRecord->ExceptionAddress);
  if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
    ee_log("    %s address %p", ep->ExceptionRecord->ExceptionInformation[0] ? "write to" : "read from",
           (void *)ep->ExceptionRecord->ExceptionInformation[1]);
  describe_addr("fault pc", ep->ExceptionRecord->ExceptionAddress);
  if (ep->ContextRecord) {
    log_ctx_stack(ep->ContextRecord);
    ee_log("  raw stack scan:");
    scan_stack(ep->ContextRecord->Esp);
  }
  fflush(NULL);
  return EXCEPTION_CONTINUE_SEARCH;
}

/* ---- last-chance crash log ---------------------------------------------------
 * A crash in the game's own code (23 Sep 2026: a write to 0xFFC inside a
 * std::deque push, Empire Earth.exe+0x51828, thread 0434) left one line in the
 * launch log: Wine's debugger cannot attach under WoW64/Rosetta.  The VEH above
 * is opt-in because it runs on every first-chance exception; a top-level
 * filter only runs when the process is about to die, so it is always on.  The
 * game's CRT installs its own filter, so SetUnhandledExceptionFilter in the exe
 * is hooked: ours stays in front and hands on to the game's. */
static LPTOP_LEVEL_EXCEPTION_FILTER g_game_filter;

static LONG WINAPI ee_last_chance(EXCEPTION_POINTERS *ep) {
  static LONG once;
  if (ep && ep->ExceptionRecord && !InterlockedExchange(&once, 1)) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    ee_log("!!! unhandled exception 0x%08lx at %p in thread %04lx", (unsigned long)er->ExceptionCode,
           er->ExceptionAddress, (unsigned long)GetCurrentThreadId());
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
      ee_log("    %s address %p", er->ExceptionInformation[0] ? "write to" : "read from",
             (void *)er->ExceptionInformation[1]);
    describe_addr("fault pc", er->ExceptionAddress);
    if (ep->ContextRecord) {
      log_ctx_stack(ep->ContextRecord);
      ee_log("  raw stack scan:");
      scan_stack(ep->ContextRecord->Esp);
    }
    fflush(NULL);
  }
  return g_game_filter ? g_game_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

static LPTOP_LEVEL_EXCEPTION_FILTER WINAPI hook_SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER f) {
  LPTOP_LEVEL_EXCEPTION_FILTER prev = g_game_filter;
  if (f != ee_last_chance)
    g_game_filter = f;
  return prev;
}

/* ---- runaway BuildPointList guard -------------------------------------------
 * 24 Sep 2026: late in a big match the game's committed memory jumped from 1.2
 * to 3.8 GB in under a minute and it crashed writing through a NULL from the
 * heap, inside Low-Level Engine's U2DSparseArrayPointContainer::BuildPointList,
 * called from Empire Earth.exe.  That routine walks each grid row's list of
 * [min, max] segments and appends a point for every y in each one (every call
 * site passes step 1), so a corrupt segment -- a wild max, or a list that loops
 * back on itself -- makes it append until the address space is gone.  (The
 * owner's earlier crash, a NULL deque block at exe+0x51828, is the same
 * exhaustion seen from another thread.)  Before each call from the game, walk
 * the rows with hard limits; if they fail, log them and return an empty list.
 * EE_POINTLIST_GUARD=0 turns it off. */
typedef void(__attribute__((thiscall)) * BuildPointListFn)(void *self, void *vec, long step);
static BuildPointListFn orig_BuildPointList;
static volatile LONG g_pl_calls, g_pl_rejects;

static int pl_ptr(const void *p) {
  uintptr_t v = (uintptr_t)p;
  return v >= 0x10000 && v < 0xfffe0000u && !(v & 3);
}

/* The container (this): row table begin/end at +8/+0xc (a VC6 vector), point
 * count at +0x18.  A row header's first dword is its segment list; a segment
 * is {min, max, ?, next}.  Returns why the rows look corrupt, or NULL. */
static const char *pl_bad(const char *self, long *count, unsigned long long *total, long *row, int *mn, int *mx) {
  char **rb = *(char ***)(self + 8), **re = *(char ***)(self + 0xc), **r;
  unsigned long nseg = 0;
  unsigned long long cap;
  *count = *(const long *)(self + 0x18);
  *total = 0;
  *row = -1;
  *mn = *mx = 0;
  if (!rb || !re)
    return NULL;
  if (!pl_ptr(rb) || re < rb || re - rb > (1 << 20))
    return "row table";
  cap = (unsigned long long)(*count > 0 ? *count : 0) * 4 + 1000000;
  for (r = rb; r < re; r++) {
    char *seg;
    if (!*r)
      continue;
    *row = (long)(r - rb);
    if (!pl_ptr(*r))
      return "row header pointer";
    for (seg = *(char **)*r; seg; seg = *(char **)(seg + 0xc)) {
      if (!pl_ptr(seg))
        return "segment pointer";
      if (++nseg > 2000000)
        return "a segment list that never ends";
      *mn = *(const int *)seg;
      *mx = *(const int *)(seg + 4);
      if (*mx < *mn)
        continue;
      if ((long long)*mx - *mn >= (1 << 20))
        return "a segment spanning over a million points";
      *total += (unsigned long long)(*mx - *mn) + 1;
      if (*total > cap)
        return "far more points than the grid holds";
    }
  }
  return NULL;
}

static void __attribute__((thiscall)) hook_BuildPointList(void *self, void *vec, long step) {
  static int want = -1;
  if (want < 0) {
    char b[8];
    want = !(GetEnvironmentVariableA("EE_POINTLIST_GUARD", b, sizeof b) > 0 && b[0] == '0');
  }
  if (!orig_BuildPointList) {
    HMODULE eng = GetModuleHandleA("Low-Level Engine.dll");
    orig_BuildPointList = eng ? (BuildPointListFn)(void *)GetProcAddress(eng, PL_EXPORT_NAME) : NULL;
    if (!orig_BuildPointList)
      return;
  }
  if (InterlockedIncrement(&g_pl_calls) == 1)
    ee_log("BuildPointList guard active (first call from the game)");
  if (want && self && vec) {
    long count, row;
    int mn, mx;
    unsigned long long total;
    const char *why = pl_bad((const char *)self, &count, &total, &row, &mn, &mx);
    if (why) {
      LONG n = InterlockedIncrement(&g_pl_rejects);
      if (n <= 20)
        ee_log("!!! BuildPointList(%p): %s (row %ld, segment [%d, %d], %llu points walked, grid count %ld, step %ld) "
               "-- returned an empty list instead of filling memory (#%ld)",
               self, why, row, mn, mx, total, count, step, (long)n);
      *(void **)((char *)vec + 8) = *(void **)((char *)vec + 4); /* VC6 vector: _Last = _First */
      return;
    }
  }
  orig_BuildPointList(self, vec, step);
}

/* ---- the render loop's Sleep(1) ------------------------------------------
 * The game's render thread (a TSThread whose loop is at 0x50fe42) ends every
 * pass with Sleep(1) -- in 2001 that handed the one CPU to the simulation
 * thread.  Under Wine on macOS it is a real ~1.2 ms sleep per frame: 10% of
 * the render thread in a big match (ee-prof, 25 Sep 2026), while the
 * simulation thread sat idle 72% of the time on its own.  In front, the pass
 * now only yields; in the background it keeps the 1 ms sleep, so a paused
 * game does not spin a core.  The world lock the two threads share hands its
 * auto-reset event to whichever waits (TSReadWriteLock), so a waiting
 * simulation still gets its turn.  EE_RENDER_SLEEP=1 restores the original. */
static void ee_render_sleep(void) {
  static DWORD next, pid;
  static int front = 1;
  DWORD now = GetTickCount();
  if ((LONG)(now - next) >= 0) {
    HWND fg = GetForegroundWindow();
    DWORD fp = 0;
    if (!pid)
      pid = GetCurrentProcessId();
    if (fg)
      GetWindowThreadProcessId(fg, &fp);
    front = fp == pid;
    next = now + 100;
  }
  if (front)
    SwitchToThread();
  else
    Sleep(1);
}

/* ---- world-lock statistics (EE_LOCK_STATS=1) -------------------------------
 * The render thread reads the game world under a TSReadWriteLock that the
 * simulation thread takes for writing while it updates.  Wrapping the three
 * lock calls (trampolines over their position-independent first bytes) logs
 * every 10 s: simulation writes per second (the game's update rate, i.e. how
 * fast animations advance), how long each write holds the world, and how
 * long readers waited for it. */
typedef void(__attribute__((thiscall)) * lockfn)(void *);
static lockfn tr_wlock, tr_wunlock, tr_rlock;
static volatile LONG ls_writes, ls_hold_us, ls_hold_max, ls_rwait_us, ls_reads;
static LARGE_INTEGER ls_freq, ls_t_acq;
static DWORD ls_next;

static LONG ls_us(LARGE_INTEGER a, LARGE_INTEGER b) { return (LONG)((b.QuadPart - a.QuadPart) * 1000000 / ls_freq.QuadPart); }

/* Writes per thread since the last stats line: the busiest writer is the
 * simulation (the loader thread writes a few times while a game loads). */
static struct { DWORD tid; LONG n; } ls_wr[8];
static void ls_count_writer(void) {
  DWORD t = GetCurrentThreadId();
  int i;
  for (i = 0; i < 8; i++) {
    if (ls_wr[i].tid == t || !ls_wr[i].tid) {
      ls_wr[i].tid = t;
      ls_wr[i].n++;
      return;
    }
  }
}
static DWORD ls_top_writer(void) {
  int i, best = 0;
  DWORD t;
  for (i = 1; i < 8; i++)
    if (ls_wr[i].n > ls_wr[best].n)
      best = i;
  t = ls_wr[best].tid;
  memset(ls_wr, 0, sizeof ls_wr);
  return t;
}

static void __attribute__((thiscall)) ls_wlock(void *self) {
  tr_wlock(self);
  ls_count_writer(); /* under the lock: one writer at a time */
  QueryPerformanceCounter(&ls_t_acq); /* one writer at a time: the lock guarantees it */
  InterlockedIncrement(&ls_writes);
}
static void __attribute__((thiscall)) ls_wunlock(void *self) {
  LARGE_INTEGER t;
  LONG held;
  DWORD now = GetTickCount();
  QueryPerformanceCounter(&t);
  held = ls_us(ls_t_acq, t);
  InterlockedExchangeAdd(&ls_hold_us, held);
  if (held > ls_hold_max)
    ls_hold_max = held;
  tr_wunlock(self);
  if ((LONG)(now - ls_next) >= 0) {
    LONG w = InterlockedExchange(&ls_writes, 0), h = InterlockedExchange(&ls_hold_us, 0), m = InterlockedExchange(&ls_hold_max, 0);
    LONG r = InterlockedExchange(&ls_reads, 0), rw = InterlockedExchange(&ls_rwait_us, 0);
    if (ls_next)
      ee_log("world lock: %.1f writes/s held %.2f ms each (max %.1f); %.1f reads/s waited %.2f ms each; writer %04lx",
             w / 10.0, w ? h / 1000.0 / w : 0.0, m / 1000.0, r / 10.0, r ? rw / 1000.0 / r : 0.0,
             (unsigned long)ls_top_writer());
    ls_next = now + 10000;
  }
}
static void __attribute__((thiscall)) ls_rlock(void *self) {
  LARGE_INTEGER a, b;
  QueryPerformanceCounter(&a);
  tr_rlock(self);
  QueryPerformanceCounter(&b);
  InterlockedExchangeAdd(&ls_rwait_us, ls_us(a, b));
  InterlockedIncrement(&ls_reads);
}

/* Jump fn to hook, keeping its first n bytes (whole instructions, position-
 * independent, checked against want) in a trampoline that continues at fn+n. */
static void *ls_hook(unsigned char *fn, const unsigned char *want, int n, void *hook) {
  unsigned char *tr;
  DWORD old;
  int rel;
  if (!fn || memcmp(fn, want, n))
    return NULL;
  tr = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
  if (!tr)
    return NULL;
  memcpy(tr, fn, n);
  tr[n] = 0xE9;
  rel = (int)((fn + n) - (tr + n + 5));
  memcpy(tr + n + 1, &rel, 4);
  if (!VirtualProtect(fn, 5, PAGE_EXECUTE_READWRITE, &old))
    return NULL;
  fn[0] = 0xE9;
  rel = (int)((unsigned char *)hook - (fn + 5));
  memcpy(fn + 1, &rel, 4);
  VirtualProtect(fn, 5, old, &old);
  FlushInstructionCache(GetCurrentProcess(), fn, 5);
  return tr;
}

static void install_lock_stats(HMODULE lle) {
  static const unsigned char wl[5] = {0x56, 0x8b, 0xf1, 0x8b, 0x0e};       /* push esi; mov esi,ecx; mov ecx,[esi] */
  static const unsigned char wu[6] = {0x56, 0x8b, 0xf1, 0x8b, 0x4e, 0x04}; /* ...; mov ecx,[esi+4] */
  static const unsigned char rl[6] = {0x56, 0x8b, 0xf1, 0x8d, 0x46, 0x0c}; /* ...; lea eax,[esi+0xc] */
  char b[8];
  if (!lle || !(GetEnvironmentVariableA("EE_LOCK_STATS", b, sizeof b) > 0 && b[0] == '1'))
    return;
  QueryPerformanceFrequency(&ls_freq);
  tr_wlock = (lockfn)ls_hook((unsigned char *)GetProcAddress(lle, "?WriteLock@TSReadWriteLock@@QAEXXZ"), wl, 5, (void *)ls_wlock);
  tr_wunlock = (lockfn)ls_hook((unsigned char *)GetProcAddress(lle, "?WriteUnlock@TSReadWriteLock@@QAEXXZ"), wu, 6, (void *)ls_wunlock);
  tr_rlock = (lockfn)ls_hook((unsigned char *)GetProcAddress(lle, "?ReadLock@TSReadWriteLock@@QAEXXZ"), rl, 6, (void *)ls_rlock);
  ee_log("world lock stats: %s", tr_wlock && tr_wunlock && tr_rlock ? "on (every 10 s)" : "could not hook all three");
}

static void install_render_sleep(void) {
  static const unsigned char want[8] = {0x6a, 0x01, 0xff, 0x15, 0x8c, 0x31, 0x82, 0x00}; /* push 1; call [Sleep] */
  unsigned char *p = (unsigned char *)0x50fee6;
  char b[8];
  DWORD old;
  int rel;
  if (GetEnvironmentVariableA("EE_RENDER_SLEEP", b, sizeof b) > 0 && b[0] == '1') {
    ee_log("render loop: Sleep(1) kept (EE_RENDER_SLEEP=1)");
    return;
  }
  if (GetModuleHandleA(NULL) != (HMODULE)0x400000 || IsBadReadPtr(p, 8) || memcmp(p, want, 8)) {
    ee_log("render loop: not the reversed Empire Earth.exe -- Sleep(1) left alone");
    return;
  }
  if (!VirtualProtect(p, 8, PAGE_EXECUTE_READWRITE, &old))
    return;
  rel = (int)((char *)ee_render_sleep - (char *)(p + 5));
  p[0] = 0xE8; /* call ee_render_sleep; nop x3 */
  memcpy(p + 1, &rel, 4);
  p[5] = p[6] = p[7] = 0x90;
  VirtualProtect(p, 8, old, &old);
  FlushInstructionCache(GetCurrentProcess(), p, 8);
  ee_log("render loop: Sleep(1) now yields while the game is in front");
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved) {
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) {
    g_self = inst;
    DisableThreadLibraryCalls(inst);
    /* Shared msvcrt => this lands in the same CRT signal table the game uses. */
    signal(SIGABRT, ee_abort_handler);
    /* The VEH identified the intermittent early crash as wow64cpu.dll+0x123d
     * (a fault inside Wine's 32<->64 transition dispatcher, reading 0x4dc9).
     * Leaving it installed makes that crash much MORE likely -- every
     * first-chance exception now runs Win32 code through the same dispatcher --
     * so it is opt-in: EE_VEH=1 to re-enable. */
    {
      char b[8];
      if (GetEnvironmentVariableA("EE_VEH", b, sizeof b) > 0 && b[0] == '1')
        AddVectoredExceptionHandler(1, ee_veh);
    }
    SetUnhandledExceptionFilter(ee_last_chance);
    install_raise_detour();
    /* The engine's hot x87 maths to SSE2 (ee-ssemath.h).  Low-Level Engine.dll
     * is a static import of the game, so it is mapped by now, and no game
     * thread has run yet -- the only safe moment to rewrite its code. */
    {
      char b[8];
      HMODULE lle = GetModuleHandleA("Low-Level Engine.dll");
      if (GetEnvironmentVariableA("EE_SSE_MATH", b, sizeof b) > 0 && b[0] == '0')
        ee_log("sse maths: off (EE_SSE_MATH=0)");
      else if (!lle)
        ee_log("sse maths: Low-Level Engine.dll is not loaded -- nothing redirected");
      else
        ee_log("sse maths: %d of %d engine functions redirected to SSE2 (x87 control word 0x%04x at start)",
               ee_ssemath_install(lle, ee_log), SSEM_N, ssem_cw());
    }
    install_render_sleep();
    install_lock_stats(GetModuleHandleA("Low-Level Engine.dll"));
    load_real();
  }
  return TRUE;
}

#define FWD(ret, name, args, params)                                                                           \
  ret WINAPI name args {                                                                                       \
    static ret(WINAPI *real) args;                                                                             \
    if (!load_real())                                                                                          \
      return (ret)0;                                                                                           \
    if (!real)                                                                                                 \
      real = (void *)GetProcAddress(g_real, #name);                                                            \
    if (!real)                                                                                                 \
      return (ret)0;                                                                                           \
    return real params;                                                                                        \
  }

FWD(BOOL, GetFileVersionInfoA, (LPCSTR a, DWORD b, DWORD c, LPVOID d), (a, b, c, d))
FWD(BOOL, GetFileVersionInfoW, (LPCWSTR a, DWORD b, DWORD c, LPVOID d), (a, b, c, d))
FWD(BOOL, GetFileVersionInfoExA, (DWORD a, LPCSTR b, DWORD c, DWORD d, LPVOID e), (a, b, c, d, e))
FWD(BOOL, GetFileVersionInfoExW, (DWORD a, LPCWSTR b, DWORD c, DWORD d, LPVOID e), (a, b, c, d, e))
FWD(DWORD, GetFileVersionInfoSizeA, (LPCSTR a, LPDWORD b), (a, b))
FWD(DWORD, GetFileVersionInfoSizeW, (LPCWSTR a, LPDWORD b), (a, b))
FWD(DWORD, GetFileVersionInfoSizeExA, (DWORD a, LPCSTR b, LPDWORD c), (a, b, c))
FWD(DWORD, GetFileVersionInfoSizeExW, (DWORD a, LPCWSTR b, LPDWORD c), (a, b, c))
FWD(BOOL, VerQueryValueA, (LPCVOID a, LPCSTR b, LPVOID *c, PUINT d), (a, b, c, d))
FWD(BOOL, VerQueryValueW, (LPCVOID a, LPCWSTR b, LPVOID *c, PUINT d), (a, b, c, d))
FWD(DWORD, VerLanguageNameA, (DWORD a, LPSTR b, DWORD c), (a, b, c))
FWD(DWORD, VerLanguageNameW, (DWORD a, LPWSTR b, DWORD c), (a, b, c))
