/* DXGI proxy: force 800x600 windowed swapchains for Empire Earth on Wine.
 * Loads dxgi_eeorig.dll (DXVK). Does not modify game binaries.
 */
#define COBJMACROS
#define CINTERFACE
#include <windows.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

#ifndef DXGI_CREATE_FACTORY_DEBUG
#define DXGI_CREATE_FACTORY_DEBUG 0x1
#endif

static HMODULE g_real;
static HRESULT(WINAPI *real_CreateDXGIFactory)(REFIID, void **);
static HRESULT(WINAPI *real_CreateDXGIFactory1)(REFIID, void **);
static HRESULT(WINAPI *real_CreateDXGIFactory2)(UINT, REFIID, void **);
static HRESULT(WINAPI *real_DXGIDeclareAdapterRemovalSupport)(void);
static HRESULT(WINAPI *real_DXGIGetDebugInterface1)(UINT, REFIID, void **);

static HRESULT(STDMETHODCALLTYPE *orig_CreateSwapChain)(void *, IUnknown *, DXGI_SWAP_CHAIN_DESC *,
                                                        IDXGISwapChain **);
static HRESULT(STDMETHODCALLTYPE *orig_CreateSwapChainForHwnd)(void *, IUnknown *, HWND,
                                                               const DXGI_SWAP_CHAIN_DESC1 *,
                                                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *,
                                                               IDXGIOutput *, IDXGISwapChain1 **);
static HRESULT(STDMETHODCALLTYPE *orig_SetFullscreenState)(IDXGISwapChain *, BOOL, IDXGIOutput *);
static HRESULT(STDMETHODCALLTYPE *orig_ResizeBuffers)(IDXGISwapChain *, UINT, UINT, UINT, DXGI_FORMAT,
                                                      UINT);
static HRESULT(STDMETHODCALLTYPE *orig_ResizeTarget)(IDXGISwapChain *, const DXGI_MODE_DESC *);

static FILE *g_log;

static void ee_log(const char *fmt, ...) {
  va_list ap;
  if (!g_log) {
    g_log = fopen("ee-dxgi.log", "a");
    if (g_log)
      setvbuf(g_log, NULL, _IONBF, 0);
  }
  fputs("ee-dxgi: ", stderr);
  if (g_log)
    fputs("ee-dxgi: ", g_log);
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
  fflush(stderr);
}

static void fix_desc(DXGI_SWAP_CHAIN_DESC *desc) {
  if (!desc)
    return;
  {
    RECT rc = {0, 0, 0, 0};
    if (desc->OutputWindow)
      GetClientRect(desc->OutputWindow, &rc);
    ee_log("CreateSwapChain in %ux%u windowed=%d hwnd=%p client=%dx%d", desc->BufferDesc.Width,
           desc->BufferDesc.Height, (int)desc->Windowed, (void *)desc->OutputWindow,
           (int)(rc.right - rc.left), (int)(rc.bottom - rc.top));
  }
  desc->BufferDesc.Width = 800;
  desc->BufferDesc.Height = 600;
  desc->Windowed = TRUE;
  desc->Flags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
}

static int(WINAPI *orig_GetSystemMetrics)(int);

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
  if (mode == ENUM_CURRENT_SETTINGS || mode == ENUM_REGISTRY_SETTINGS || mode == 0) {
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

static LONG WINAPI hook_ChangeDisplaySettingsA(DEVMODEA *dm, DWORD flags) {
  (void)dm;
  (void)flags;
  ee_log("ChangeDisplaySettingsA -> SUCCESS (800x600 fake)");
  return DISP_CHANGE_SUCCESSFUL;
}

static LONG WINAPI hook_ChangeDisplaySettingsExA(const char *dev, DEVMODEA *dm, HWND hwnd, DWORD flags,
                                                 void *param) {
  (void)dev;
  (void)dm;
  (void)hwnd;
  (void)flags;
  (void)param;
  return DISP_CHANGE_SUCCESSFUL;
}

static LONG WINAPI hook_ChangeDisplaySettingsExW(const wchar_t *dev, DEVMODEW *dm, HWND hwnd, DWORD flags,
                                                 void *param) {
  (void)dev;
  (void)dm;
  (void)hwnd;
  (void)flags;
  (void)param;
  return DISP_CHANGE_SUCCESSFUL;
}

static BOOL WINAPI hook_EnumDisplaySettingsW(const wchar_t *dev, DWORD mode, DEVMODEW *dm) {
  (void)dev;
  if (!dm)
    return FALSE;
  if (mode == ENUM_CURRENT_SETTINGS || mode == ENUM_REGISTRY_SETTINGS || mode == 0) {
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

static LONG WINAPI hook_ChangeDisplaySettingsW(DEVMODEW *dm, DWORD flags) {
  (void)dm;
  (void)flags;
  ee_log("ChangeDisplaySettingsW -> SUCCESS");
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

static void install_mode_hooks(void) {
  static int once;
  unsigned i;
  const char *mods[] = {NULL, "ddraw.dll", "D3DImm.dll", "DX7HRDisplay.dll", "DX7HRTnLDisplay.dll"};
  HMODULE user;
  if (once)
    return;
  once = 1;
  user = GetModuleHandleA("user32.dll");
  if (user && !orig_GetSystemMetrics)
    orig_GetSystemMetrics = (void *)GetProcAddress(user, "GetSystemMetrics");
  for (i = 0; i < sizeof mods / sizeof mods[0]; i++) {
    HMODULE m = mods[i] ? GetModuleHandleA(mods[i]) : GetModuleHandleA(NULL);
    if (!m)
      continue;
    patch_iat(m, "USER32.dll", "EnumDisplaySettingsA", (void *)hook_EnumDisplaySettingsA);
    patch_iat(m, "user32.dll", "EnumDisplaySettingsA", (void *)hook_EnumDisplaySettingsA);
    patch_iat(m, "USER32.dll", "EnumDisplaySettingsExA", (void *)hook_EnumDisplaySettingsExA);
    patch_iat(m, "user32.dll", "EnumDisplaySettingsExA", (void *)hook_EnumDisplaySettingsExA);
    patch_iat(m, "USER32.dll", "ChangeDisplaySettingsA", (void *)hook_ChangeDisplaySettingsA);
    patch_iat(m, "user32.dll", "ChangeDisplaySettingsA", (void *)hook_ChangeDisplaySettingsA);
    patch_iat(m, "USER32.dll", "ChangeDisplaySettingsExA", (void *)hook_ChangeDisplaySettingsExA);
    patch_iat(m, "user32.dll", "ChangeDisplaySettingsExA", (void *)hook_ChangeDisplaySettingsExA);
    patch_iat(m, "USER32.dll", "EnumDisplaySettingsW", (void *)hook_EnumDisplaySettingsW);
    patch_iat(m, "user32.dll", "EnumDisplaySettingsW", (void *)hook_EnumDisplaySettingsW);
    patch_iat(m, "USER32.dll", "EnumDisplaySettingsExW", (void *)hook_EnumDisplaySettingsExW);
    patch_iat(m, "user32.dll", "EnumDisplaySettingsExW", (void *)hook_EnumDisplaySettingsExW);
    patch_iat(m, "USER32.dll", "ChangeDisplaySettingsW", (void *)hook_ChangeDisplaySettingsW);
    patch_iat(m, "user32.dll", "ChangeDisplaySettingsW", (void *)hook_ChangeDisplaySettingsW);
    patch_iat(m, "USER32.dll", "ChangeDisplaySettingsExW", (void *)hook_ChangeDisplaySettingsExW);
    patch_iat(m, "user32.dll", "ChangeDisplaySettingsExW", (void *)hook_ChangeDisplaySettingsExW);
    patch_iat(m, "USER32.dll", "GetSystemMetrics", (void *)hook_GetSystemMetrics);
    patch_iat(m, "user32.dll", "GetSystemMetrics", (void *)hook_GetSystemMetrics);
  }
}

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

static HRESULT STDMETHODCALLTYPE hook_SetFullscreenState(IDXGISwapChain *sc, BOOL fs, IDXGIOutput *out) {
  ee_log("SetFullscreenState(%d) -> FALSE", (int)fs);
  (void)fs;
  (void)out;
  return orig_SetFullscreenState ? orig_SetFullscreenState(sc, FALSE, NULL) : S_OK;
}

static HRESULT STDMETHODCALLTYPE hook_ResizeBuffers(IDXGISwapChain *sc, UINT count, UINT w, UINT h,
                                                    DXGI_FORMAT fmt, UINT flags) {
  if (w < 320 || h < 240) {
    ee_log("ResizeBuffers %ux%u -> 800x600", w, h);
    w = 800;
    h = 600;
  }
  return orig_ResizeBuffers(sc, count, w, h, fmt, flags);
}

static HRESULT STDMETHODCALLTYPE hook_ResizeTarget(IDXGISwapChain *sc, const DXGI_MODE_DESC *mode) {
  DXGI_MODE_DESC local;
  if (mode) {
    local = *mode;
    if (local.Width < 320 || local.Height < 240) {
      ee_log("ResizeTarget %ux%u -> 800x600", local.Width, local.Height);
      local.Width = 800;
      local.Height = 600;
    }
    mode = &local;
  }
  return orig_ResizeTarget(sc, mode);
}

static void hook_swapchain(IDXGISwapChain *sc) {
  void **vt;
  if (!sc)
    return;
  vt = writable_vt(sc, 18);
  if (!vt)
    return;
  if (!orig_SetFullscreenState)
    orig_SetFullscreenState = (void *)vt[10];
  if (!orig_ResizeBuffers)
    orig_ResizeBuffers = (void *)vt[13];
  if (!orig_ResizeTarget)
    orig_ResizeTarget = (void *)vt[14];
  vt[10] = (void *)hook_SetFullscreenState;
  vt[13] = (void *)hook_ResizeBuffers;
  vt[14] = (void *)hook_ResizeTarget;
}

static HRESULT STDMETHODCALLTYPE hook_CreateSwapChain(void *factory, IUnknown *device,
                                                      DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **out) {
  HRESULT hr;
  DXGI_SWAP_CHAIN_DESC local;
  HWND original = NULL;
  if (desc) {
    local = *desc;
    original = local.OutputWindow;
    install_mode_hooks();
    fix_desc(&local);
    desc = &local;
  }
  hr = orig_CreateSwapChain(factory, device, desc, out);
  if (FAILED(hr) && desc && original && local.OutputWindow != original) {
    ee_log("retarget failed 0x%08lx; retry original hwnd %p", (unsigned long)hr, (void *)original);
    local.OutputWindow = original;
    hr = orig_CreateSwapChain(factory, device, &local, out);
  }
  ee_log("CreateSwapChain hr=0x%08lx hwnd=%p", (unsigned long)hr, desc ? (void *)desc->OutputWindow : NULL);
  return hr;
}

static HRESULT STDMETHODCALLTYPE hook_CreateSwapChainForHwnd(void *factory, IUnknown *device, HWND hwnd,
                                                             const DXGI_SWAP_CHAIN_DESC1 *desc,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs,
                                                             IDXGIOutput *restrict_out,
                                                             IDXGISwapChain1 **out) {
  HRESULT hr;
  DXGI_SWAP_CHAIN_DESC1 local;
  DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs_local;
  const DXGI_SWAP_CHAIN_DESC1 *use_desc = desc;
  const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *use_fs = fs;
  if (desc) {
    local = *desc;
    ee_log("CreateSwapChainForHwnd in %ux%u hwnd=%p", local.Width, local.Height, (void *)hwnd);
    local.Width = 800;
    local.Height = 600;
    use_desc = &local;
  }
  if (fs) {
    fs_local = *fs;
    fs_local.Windowed = TRUE;
    use_fs = &fs_local;
  }
  hr = orig_CreateSwapChainForHwnd(factory, device, hwnd, use_desc, use_fs, restrict_out, out);
  return hr;
}

static void hook_factory(void *factory) {
  void **vt;
  if (!factory)
    return;
  /* Factory / Factory1 / Factory2 share CreateSwapChain at slot 10.
   * Factory2 CreateSwapChainForHwnd is slot 15. */
  vt = writable_vt(factory, 20);
  if (!vt)
    return;
  if (!orig_CreateSwapChain)
    orig_CreateSwapChain = (void *)vt[10];
  vt[10] = (void *)hook_CreateSwapChain;
  /* Do not patch slot 15: IDXGIFactory is shorter than Factory2; OOB hooks crash Wine. */
  ee_log("hooked IDXGIFactory CreateSwapChain");
}

static int load_real(void) {
  char path[MAX_PATH];
  DWORD n;
  char *slash;
  if (g_real)
    return 1;
  n = GetModuleFileNameA((HINSTANCE)&__ImageBase, path, MAX_PATH);
  (void)n;
  slash = strrchr(path, '\\');
  if (!slash)
    slash = strrchr(path, '/');
  if (!slash)
    return 0;
  strcpy(slash + 1, "dxgi_eeorig.dll");
  g_real = LoadLibraryA(path);
  if (!g_real) {
    ee_log("LoadLibrary %s failed %lu", path, (unsigned long)GetLastError());
    return 0;
  }
  real_CreateDXGIFactory = (void *)GetProcAddress(g_real, "CreateDXGIFactory");
  real_CreateDXGIFactory1 = (void *)GetProcAddress(g_real, "CreateDXGIFactory1");
  real_CreateDXGIFactory2 = (void *)GetProcAddress(g_real, "CreateDXGIFactory2");
  real_DXGIDeclareAdapterRemovalSupport = (void *)GetProcAddress(g_real, "DXGIDeclareAdapterRemovalSupport");
  real_DXGIGetDebugInterface1 = (void *)GetProcAddress(g_real, "DXGIGetDebugInterface1");
  ee_log("wrapping %s", path);
  return 1;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved) {
  (void)inst;
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) {
    load_real();
    install_mode_hooks();
  }
  return TRUE;
}

HRESULT WINAPI CreateDXGIFactory(REFIID riid, void **factory) {
  HRESULT hr;
  if (!load_real() || !real_CreateDXGIFactory)
    return E_FAIL;
  hr = real_CreateDXGIFactory(riid, factory);
  if (SUCCEEDED(hr) && factory)
    hook_factory(*factory);
  return hr;
}

HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void **factory) {
  HRESULT hr;
  if (!load_real() || !real_CreateDXGIFactory1)
    return CreateDXGIFactory(riid, factory);
  hr = real_CreateDXGIFactory1(riid, factory);
  if (SUCCEEDED(hr) && factory)
    hook_factory(*factory);
  return hr;
}

HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void **factory) {
  HRESULT hr;
  if (!load_real() || !real_CreateDXGIFactory2)
    return CreateDXGIFactory1(riid, factory);
  hr = real_CreateDXGIFactory2(flags, riid, factory);
  if (SUCCEEDED(hr) && factory)
    hook_factory(*factory);
  return hr;
}

HRESULT WINAPI DXGIDeclareAdapterRemovalSupport(void) {
  if (!load_real() || !real_DXGIDeclareAdapterRemovalSupport)
    return S_OK;
  return real_DXGIDeclareAdapterRemovalSupport();
}

HRESULT WINAPI DXGIGetDebugInterface1(UINT flags, REFIID riid, void **out) {
  if (!load_real() || !real_DXGIGetDebugInterface1)
    return E_NOINTERFACE;
  return real_DXGIGetDebugInterface1(flags, riid, out);
}
