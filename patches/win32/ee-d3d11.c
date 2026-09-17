/* D3D11 proxy: force 800x600 windowed on D3D11CreateDeviceAndSwapChain.
 * Loads d3d11_eeorig.dll (DXVK). Does not modify game binaries.
 */
#define COBJMACROS
#define CINTERFACE
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

static HMODULE g_real;
static HRESULT(WINAPI *real_D3D11CreateDeviceAndSwapChain)(
    IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
    const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D11Device **, D3D_FEATURE_LEVEL *,
    ID3D11DeviceContext **);
static HRESULT(WINAPI *real_D3D11CreateDevice)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
                                               const D3D_FEATURE_LEVEL *, UINT, UINT, ID3D11Device **,
                                               D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
static HRESULT(WINAPI *real_D3D11CoreCreateDevice)(void *, void *, UINT, const D3D_FEATURE_LEVEL *,
                                                   UINT, ID3D11Device **);
static HRESULT(WINAPI *real_D3D11On12CreateDevice)(IUnknown *, UINT, const D3D_FEATURE_LEVEL *, UINT,
                                                   IUnknown **, UINT, UINT, ID3D11Device **,
                                                   ID3D11DeviceContext **, D3D_FEATURE_LEVEL *);

static void ee_log(const char *fmt, ...) {
  va_list ap;
  fputs("ee-d3d11: ", stderr);
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  fflush(stderr);
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
  strcpy(slash + 1, "d3d11_eeorig.dll");
  g_real = LoadLibraryA(path);
  if (!g_real) {
    ee_log("LoadLibrary %s failed %lu", path, (unsigned long)GetLastError());
    return 0;
  }
  real_D3D11CreateDeviceAndSwapChain = (void *)GetProcAddress(g_real, "D3D11CreateDeviceAndSwapChain");
  real_D3D11CreateDevice = (void *)GetProcAddress(g_real, "D3D11CreateDevice");
  real_D3D11CoreCreateDevice = (void *)GetProcAddress(g_real, "D3D11CoreCreateDevice");
  real_D3D11On12CreateDevice = (void *)GetProcAddress(g_real, "D3D11On12CreateDevice");
  ee_log("wrapping %s", path);
  return 1;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved) {
  (void)inst;
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH)
    load_real();
  return TRUE;
}

HRESULT WINAPI D3D11CreateDeviceAndSwapChain(IDXGIAdapter *adapter, D3D_DRIVER_TYPE type, HMODULE software,
                                             UINT flags, const D3D_FEATURE_LEVEL *levels, UINT nlevels,
                                             UINT sdk, const DXGI_SWAP_CHAIN_DESC *desc,
                                             IDXGISwapChain **swapchain, ID3D11Device **device,
                                             D3D_FEATURE_LEVEL *out_level, ID3D11DeviceContext **ctx) {
  DXGI_SWAP_CHAIN_DESC local;
  const DXGI_SWAP_CHAIN_DESC *use = desc;
  if (!load_real() || !real_D3D11CreateDeviceAndSwapChain)
    return E_FAIL;
  if (desc) {
    local = *desc;
    ee_log("DeviceAndSwapChain in %ux%u windowed=%d hwnd=%p", local.BufferDesc.Width,
           local.BufferDesc.Height, (int)local.Windowed, (void *)local.OutputWindow);
    local.BufferDesc.Width = 800;
    local.BufferDesc.Height = 600;
    local.Windowed = TRUE;
    local.Flags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    {
      HWND host = FindWindowA("EmpireEarthMacHost", NULL);
      if (host)
        local.OutputWindow = host;
    }
    use = &local;
  }
  return real_D3D11CreateDeviceAndSwapChain(adapter, type, software, flags, levels, nlevels, sdk, use,
                                            swapchain, device, out_level, ctx);
}

HRESULT WINAPI D3D11CreateDevice(IDXGIAdapter *adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags,
                                 const D3D_FEATURE_LEVEL *levels, UINT nlevels, UINT sdk,
                                 ID3D11Device **device, D3D_FEATURE_LEVEL *out_level,
                                 ID3D11DeviceContext **ctx) {
  if (!load_real() || !real_D3D11CreateDevice)
    return E_FAIL;
  return real_D3D11CreateDevice(adapter, type, software, flags, levels, nlevels, sdk, device, out_level,
                                ctx);
}

HRESULT WINAPI D3D11CoreCreateDevice(void *a, void *b, UINT c, const D3D_FEATURE_LEVEL *d, UINT e,
                                     ID3D11Device **f) {
  if (!load_real() || !real_D3D11CoreCreateDevice)
    return E_FAIL;
  return real_D3D11CoreCreateDevice(a, b, c, d, e, f);
}

HRESULT WINAPI D3D11On12CreateDevice(IUnknown *a, UINT b, const D3D_FEATURE_LEVEL *c, UINT d, IUnknown **e,
                                     UINT f, UINT g, ID3D11Device **h, ID3D11DeviceContext **i,
                                     D3D_FEATURE_LEVEL *j) {
  if (!load_real() || !real_D3D11On12CreateDevice)
    return E_NOINTERFACE;
  return real_D3D11On12CreateDevice(a, b, c, d, e, f, g, h, i, j);
}
