/* MoltenVK + win32u wrapper for Empire Earth on Wine/macOS.
 *
 * Two layers, because Wine 11 win32u overwrites MoltenVK surface caps with
 * NtUserGetClientRect / GetPresentRect after the ICD returns:
 *   1. Rewrite tiny Vulkan extents and CAMetalLayer drawableSize to 800x600.
 *   2. Detour NtUserCallHwndParam so tiny HWND client/present rects are 800x600
 *      and actually grow the Win32 window (DXVK exclusive-FS uses that size).
 *
 * Does not modify game binaries. Linked as libMoltenVK.dylib in front of
 * libMoltenVK.real.dylib.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#include <libkern/OSCacheControl.h>

typedef int32_t VkResult;
typedef uint32_t VkFlags;
typedef uint32_t VkBool32;
typedef uint32_t VkStructureType;
typedef struct VkInstance_T *VkInstance;
typedef struct VkPhysicalDevice_T *VkPhysicalDevice;
typedef struct VkDevice_T *VkDevice;
typedef struct VkSurfaceKHR_T *VkSurfaceKHR;
typedef struct VkSwapchainKHR_T *VkSwapchainKHR;
typedef struct VkAllocationCallbacks VkAllocationCallbacks;
typedef void (*PFN_vkVoidFunction)(void);

typedef struct {
  uint32_t width;
  uint32_t height;
} VkExtent2D;

typedef struct {
  uint32_t minImageCount;
  uint32_t maxImageCount;
  VkExtent2D currentExtent;
  VkExtent2D minImageExtent;
  VkExtent2D maxImageExtent;
  uint32_t maxImageArrayLayers;
  VkFlags supportedTransforms;
  uint32_t currentTransform;
  VkFlags supportedCompositeAlpha;
  VkFlags supportedUsageFlags;
} VkSurfaceCapabilitiesKHR;

typedef struct {
  VkStructureType sType;
  void *pNext;
  VkSurfaceCapabilitiesKHR surfaceCapabilities;
} VkSurfaceCapabilities2KHR;

typedef struct {
  VkStructureType sType;
  const void *pNext;
  VkFlags flags;
  VkSurfaceKHR surface;
  uint32_t minImageCount;
  uint32_t imageFormat;
  uint32_t imageColorSpace;
  VkExtent2D imageExtent;
  uint32_t imageArrayLayers;
  VkFlags imageUsage;
  uint32_t imageSharingMode;
  uint32_t queueFamilyIndexCount;
  const uint32_t *pQueueFamilyIndices;
  uint32_t preTransform;
  uint32_t compositeAlpha;
  uint32_t presentMode;
  VkBool32 clipped;
  VkSwapchainKHR oldSwapchain;
} VkSwapchainCreateInfoKHR;

typedef struct {
  VkStructureType sType;
  void *pNext;
} VkBaseOutStructure;

typedef struct {
  VkStructureType sType;
  const void *pNext;
  uint32_t fullScreenExclusive;
} VkSurfaceFullScreenExclusiveInfoEXT;

typedef struct {
  VkStructureType sType;
  void *pNext;
  VkBool32 fullScreenExclusiveSupported;
} VkSurfaceCapabilitiesFullScreenExclusiveEXT;

#define VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR 1000119001u
#define VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT 1000255000u
#define VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_FULL_SCREEN_EXCLUSIVE_EXT 1000255002u
#define VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT 2u
#define VK_EXTENT_DONTCARE 0xFFFFFFFFu

enum {
  /* include/ntuser.h, wine-11.0 */
  NtUserCallHwndParam_GetWindowRect = 13,
  NtUserCallHwndParam_GetClientRect = 14,
  NtUserCallHwndParam_GetPresentRect = 15,
};

#define SWP_NOMOVE 0x0002
#define SWP_NOZORDER 0x0004
#define SWP_NOACTIVATE 0x0010
#define SWP_SHOWWINDOW 0x0040
#define SW_SHOW 5

typedef struct {
  int32_t left;
  int32_t top;
  int32_t right;
  int32_t bottom;
} EeRect;

typedef struct {
  EeRect *rect;
  uint32_t dpi;
} EeGetWindowRectsParams;

typedef uintptr_t (*NtUserCallHwndParamFn)(void *hwnd, uintptr_t param, unsigned int code);
typedef int (*NtUserSetWindowPosFn)(void *hwnd, void *after, int x, int y, int cx, int cy,
                                    unsigned int flags);
typedef int (*NtUserShowWindowFn)(void *hwnd, int cmd);
typedef void *(*ClientSurfaceCreateFn)(unsigned int size, const void *funcs, void *hwnd);

static NtUserCallHwndParamFn orig_call_hwnd_param;
static NtUserSetWindowPosFn orig_set_window_pos;
static NtUserShowWindowFn orig_show_window;
static ClientSurfaceCreateFn orig_client_surface_create;
static __thread int g_in_win32u_hook;
static void *g_real;

static int rect_too_small(const EeRect *rect) {
  int32_t w, h;
  if (!rect)
    return 0;
  w = rect->right - rect->left;
  h = rect->bottom - rect->top;
  if (w < 0)
    w = -w;
  if (h < 0)
    h = -h;
  return w < 320 || h < 240;
}

/* ---- the shim's idea of "the right size" ---------------------------------
 * Everything below was written when the game's window came up degenerate (1x1,
 * 16x16, a 640x260 splash) and 800x600 was a safe constant to force.  Since the
 * Wine virtual desktop started working the window is a healthy 1024x768, and
 * forcing 800x600 on a healthy surface is now the ONLY remaining size bug: the
 * game renders 1024x768, the shim clamps the Vulkan surface and the Metal
 * drawable to 800x600, and kCAGravityResize stretches that back up.
 * EE_VKFIX_SIZE=WxH overrides; EE_VKFIX_FORCE_EXTENT=1 restores the old
 * unconditional rewrite. */
static unsigned g_fix_w, g_fix_h;

static void load_fix_size(void) {
  const char *e;
  if (g_fix_w)
    return;
  g_fix_w = 1024;
  g_fix_h = 768;
  e = getenv("EE_VKFIX_SIZE");
  if (e) {
    unsigned w = 0, h = 0;
    if (sscanf(e, "%ux%u", &w, &h) == 2 && w >= 320 && h >= 240) {
      g_fix_w = w;
      g_fix_h = h;
    }
  }
  fprintf(stderr, "ee-vkfix: fallback size %ux%u\n", g_fix_w, g_fix_h);
  fflush(stderr);
}

static int force_extent(void) {
  static int v = -1;
  if (v < 0) {
    const char *e = getenv("EE_VKFIX_FORCE_EXTENT");
    v = (e && e[0] == '1') ? 1 : 0;
  }
  return v;
}

static void grow_hwnd(void *hwnd) {
  if (!hwnd || !orig_set_window_pos || g_in_win32u_hook)
    return;
  load_fix_size();
  g_in_win32u_hook = 1;
  orig_set_window_pos(hwnd, NULL, 0, 0, (int)g_fix_w, (int)g_fix_h,
                      SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
  if (orig_show_window)
    orig_show_window(hwnd, SW_SHOW);
  g_in_win32u_hook = 0;
}

static void rewrite_rect(EeRect *rect, unsigned int code) {
  int32_t w, h;
  if (!rect_too_small(rect))
    return;
  w = rect->right - rect->left;
  h = rect->bottom - rect->top;
  load_fix_size();
  if (code == NtUserCallHwndParam_GetWindowRect) {
    rect->right = rect->left + (int32_t)g_fix_w;
    rect->bottom = rect->top + (int32_t)g_fix_h;
  } else {
    rect->left = 0;
    rect->top = 0;
    rect->right = (int32_t)g_fix_w;
    rect->bottom = (int32_t)g_fix_h;
  }
  fprintf(stderr, "ee-vkfix: hwnd rect %dx%d -> %ux%u (code %u)\n", w, h, g_fix_w, g_fix_h, code);
  fflush(stderr);
}

static uintptr_t hook_NtUserCallHwndParam(void *hwnd, uintptr_t param, unsigned int code) {
  uintptr_t result;
  EeGetWindowRectsParams *params;
  result = orig_call_hwnd_param(hwnd, param, code);
  if (g_in_win32u_hook)
    return result;
  if (code != NtUserCallHwndParam_GetWindowRect && code != NtUserCallHwndParam_GetClientRect &&
      code != NtUserCallHwndParam_GetPresentRect)
    return result;
  if (!result || !param)
    return result;
  params = (EeGetWindowRectsParams *)param;
  if (!params->rect)
    return result;
  if (rect_too_small(params->rect)) {
    rewrite_rect(params->rect, code);
    /* Do not SetWindowPos here: Wine macdrv is not reentrant during Vulkan GetCaps. */
  }
  return result;
}

static void *hook_client_surface_create(unsigned int size, const void *funcs, void *hwnd) {
  grow_hwnd(hwnd);
  return orig_client_surface_create(size, funcs, hwnd);
}

static int make_writable(void *page, size_t size) {
  if (mprotect(page, size, PROT_READ | PROT_WRITE) != 0)
    return -1;
  return 0;
}

static int make_rx(void *page, size_t size) {
  if (mprotect(page, size, PROT_READ | PROT_EXEC) != 0)
    return -1;
  sys_icache_invalidate(page, size);
  sys_dcache_flush(page, size);
  return 0;
}

/* Patch dest to jump to hook. Copy stolen bytes into a trampoline that then
 * jumps back to dest+stolen. orig_out receives the trampoline. */
static int install_abs_jump(void *dest, void *hook, size_t stolen, void **orig_out) {
  uint8_t *src = dest;
  uint8_t *tramp;
  uint8_t *page;
  uintptr_t addr = (uintptr_t)dest;
  size_t page_size = (size_t)getpagesize();
  uintptr_t page_start = addr & ~(uintptr_t)(page_size - 1);
  size_t page_len = page_size;
  uint8_t jmp[12];
  size_t i;

  if (stolen < 12 || stolen > 32)
    return -1;
  if ((addr + stolen - 1) / page_size != addr / page_size)
    page_len = page_size * 2;

  tramp = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
  if (tramp == MAP_FAILED)
    return -1;
  memcpy(tramp, src, stolen);
  tramp[stolen + 0] = 0x48;
  tramp[stolen + 1] = 0xb8;
  {
    uintptr_t back = (uintptr_t)src + stolen;
    memcpy(tramp + stolen + 2, &back, sizeof(back));
  }
  tramp[stolen + 10] = 0xff;
  tramp[stolen + 11] = 0xe0;
  if (make_rx(tramp, page_size) != 0) {
    munmap(tramp, page_size);
    return -1;
  }

  jmp[0] = 0x48;
  jmp[1] = 0xb8;
  memcpy(jmp + 2, &hook, sizeof(void *));
  jmp[10] = 0xff;
  jmp[11] = 0xe0;

  page = (uint8_t *)page_start;
  if (make_writable(page, page_len) != 0) {
    munmap(tramp, page_size);
    return -1;
  }
  memcpy(src, jmp, 12);
  for (i = 12; i < stolen; i++)
    src[i] = 0x90;
  if (make_rx(page, page_len) != 0) {
    /* Best-effort restore is worse than leaving the jump in place. */
  }
  *orig_out = tramp;
  return 0;
}

static void install_win32u_hooks(void) {
  void *call;
  void *setpos;
  void *show;
  void *create;
  void *orig;

  call = dlsym(RTLD_DEFAULT, "NtUserCallHwndParam");
  setpos = dlsym(RTLD_DEFAULT, "NtUserSetWindowPos");
  show = dlsym(RTLD_DEFAULT, "NtUserShowWindow");
  create = dlsym(RTLD_DEFAULT, "client_surface_create");
  orig_set_window_pos = setpos;
  orig_show_window = show;
  (void)hook_client_surface_create;
  (void)orig_client_surface_create;
  if (!call) {
    fprintf(stderr, "ee-vkfix: NtUserCallHwndParam not found (%s)\n", dlerror());
    fflush(stderr);
    return;
  }
  /* Wine 11.0 _NtUserCallHwndParam prologue is 17 bytes through `sub $0x68,%rsp`. */
  if (install_abs_jump(call, (void *)hook_NtUserCallHwndParam, 17, &orig) != 0) {
    fprintf(stderr, "ee-vkfix: failed to detour NtUserCallHwndParam (errno %d)\n", errno);
    fflush(stderr);
    orig_call_hwnd_param = (NtUserCallHwndParamFn)call;
    return;
  }
  orig_call_hwnd_param = (NtUserCallHwndParamFn)orig;
  fprintf(stderr, "ee-vkfix: hooked NtUserCallHwndParam @ %p\n", call);
  fflush(stderr);
  (void)create;
}

static void force_metal_layers(void) {
  void (^work)(void) = ^{
    @autoreleasepool {
      static int floated;
      if (!NSApp)
        return;
      for (NSWindow *window in [NSApp windows]) {
        BOOL has_metal = NO;
        if (!window.contentView)
          continue;
        NSSize win_size = window.frame.size;
        NSMutableArray<NSView *> *stack = [NSMutableArray arrayWithObject:window.contentView];
        while (stack.count) {
          NSView *view = stack.lastObject;
          [stack removeLastObject];
          if ([view.layer isKindOfClass:[CAMetalLayer class]]) {
            has_metal = YES;
            view.hidden = NO;
            CAMetalLayer *metal = (CAMetalLayer *)view.layer;
            NSSize bounds = metal.bounds.size;
            NSSize host = view.bounds.size;
            if (host.width < 1 || host.height < 1)
              host = win_size;
            /* Keep the layer inside the real NSWindow. Forcing 800x600 layer
             * bounds on a 640x260 splash crops the menu off-screen. Growing the
             * window itself page-faults wow64cpu (7BF21139). Scale instead. */
            metal.contentsScale = 1.0;
            metal.anchorPoint = CGPointZero;
            /* ResizeAspect, not Resize: if the drawable and the view ever
             * disagree, letterbox rather than stretch -- a 4:3 render blown
             * into a 16:10 panel is about 15% too wide. */
            metal.contentsGravity = kCAGravityResizeAspect;
            metal.frame = CGRectMake(0, 0, host.width, host.height);
            metal.bounds = CGRectMake(0, 0, host.width, host.height);
            metal.drawableSize = CGSizeMake(host.width, host.height);
            {
              static double last_w, last_h;
              if (host.width != last_w || host.height != last_h) {
                fprintf(stderr, "ee-vkfix: CAMetalLayer was %gx%g host %gx%g win %gx%g; drawable %gx%g\n",
                        bounds.width, bounds.height, host.width, host.height, win_size.width,
                        win_size.height, host.width, host.height);
                fflush(stderr);
                last_w = host.width;
                last_h = host.height;
              }
            }
          }
          if (view.subviews.count)
            [stack addObjectsFromArray:view.subviews];
        }
        if (has_metal && !floated && win_size.width >= 700 && win_size.height >= 500) {
          const char *want_float = getenv("EE_VKFIX_FLOAT");
          floated = 1;
          /* NSFloatingWindowLevel pins the game above every other application,
           * which is half of why the window could not be escaped.  It was added
           * when the window kept being lost; the ddraw-side watchdog covers that
           * now, so this is opt-in. */
          if (want_float && want_float[0] == '1')
            [window setLevel:NSFloatingWindowLevel];
          [window orderFrontRegardless];
          [NSApp activateIgnoringOtherApps:YES];
          fprintf(stderr, "ee-vkfix: brought Metal window to front frame=%gx%g\n", win_size.width,
                  win_size.height);
          fflush(stderr);
        }
      }
    }
  };
  if ([NSThread isMainThread])
    work();
  else
    dispatch_sync(dispatch_get_main_queue(), work);
}

static void *real_sym(const char *name) {
  void *sym = dlsym(g_real, name);
  if (!sym)
    fprintf(stderr, "ee-vkfix: missing %s (%s)\n", name, dlerror());
  return sym;
}

__attribute__((constructor)) static void ee_vkfix_init(void) {
  Dl_info info;
  char path[4096];
  char *slash;
  if (!dladdr((void *)ee_vkfix_init, &info) || !info.dli_fname) {
    fprintf(stderr, "ee-vkfix: dladdr failed\n");
    return;
  }
  snprintf(path, sizeof path, "%s", info.dli_fname);
  slash = strrchr(path, '/');
  if (!slash)
    return;
  snprintf(slash + 1, sizeof(path) - (size_t)(slash + 1 - path), "libMoltenVK.real.dylib");
  g_real = dlopen(path, RTLD_NOW);
  if (!g_real)
    fprintf(stderr, "ee-vkfix: dlopen %s failed: %s\n", path, dlerror());
  else {
    fprintf(stderr, "ee-vkfix: wrapping %s\n", path);
    fflush(stderr);
  }
  /* Runtime detours of win32u crash under Rosetta (self-modifying x86_64).
   * HWND extent overwrite is NOPed on disk in install_win32u_extent_patch(). */
}

static void fix_caps(VkSurfaceCapabilitiesKHR *caps) {
  uint32_t w = caps->currentExtent.width;
  uint32_t h = caps->currentExtent.height;
  int degenerate = (w == 0xFFFFFFFFu || h == 0xFFFFFFFFu || w < 320 || h < 240);
  load_fix_size();
  /* Only rewrite an extent the compositor could not have meant.  A healthy
   * 1024x768 surface must pass through untouched or the whole frame is rendered
   * at the fallback size and stretched back up. */
  if (degenerate || force_extent()) {
    if (w != g_fix_w || h != g_fix_h) {
      static uint32_t last_w, last_h;
      if (w != last_w || h != last_h) { /* the unthrottled version wrote 10k+ lines a run */
        fprintf(stderr, "ee-vkfix: surface %ux%u -> %ux%u\n", w, h, g_fix_w, g_fix_h);
        fflush(stderr);
        last_w = w;
        last_h = h;
      }
      caps->currentExtent.width = g_fix_w;
      caps->currentExtent.height = g_fix_h;
    }
  }
  if (caps->currentExtent.width == g_fix_w && caps->currentExtent.height == g_fix_h) {
    if (caps->minImageExtent.width < g_fix_w)
      caps->minImageExtent.width = g_fix_w;
    if (caps->minImageExtent.height < g_fix_h)
      caps->minImageExtent.height = g_fix_h;
  } else {
    if (caps->minImageExtent.width <= 1)
      caps->minImageExtent.width = 1;
    if (caps->minImageExtent.height <= 1)
      caps->minImageExtent.height = 1;
  }
  if (caps->maxImageExtent.width < g_fix_w)
    caps->maxImageExtent.width = 16384;
  if (caps->maxImageExtent.height < g_fix_h)
    caps->maxImageExtent.height = 16384;
}

static void fix_exclusive_out(void *pNext) {
  for (VkBaseOutStructure *node = pNext; node; node = node->pNext) {
    if (node->sType == VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_FULL_SCREEN_EXCLUSIVE_EXT) {
      VkSurfaceCapabilitiesFullScreenExclusiveEXT *ext =
          (VkSurfaceCapabilitiesFullScreenExclusiveEXT *)node;
      ext->fullScreenExclusiveSupported = 0;
    }
  }
}

VkResult vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice gpu, VkSurfaceKHR surface,
                                                   VkSurfaceCapabilitiesKHR *caps) {
  VkResult (*real)(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR *) =
      real_sym("vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
  force_metal_layers();
  VkResult r = real ? real(gpu, surface, caps) : -1;
  if (r == 0 && caps) {
    fprintf(stderr, "ee-vkfix: caps %ux%u\n", caps->currentExtent.width, caps->currentExtent.height);
    fflush(stderr);
    fix_caps(caps);
  }
  return r;
}

VkResult vkGetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice gpu, const void *info,
                                                    VkSurfaceCapabilities2KHR *caps) {
  VkResult (*real)(VkPhysicalDevice, const void *, VkSurfaceCapabilities2KHR *) =
      real_sym("vkGetPhysicalDeviceSurfaceCapabilities2KHR");
  force_metal_layers();
  VkResult r = real ? real(gpu, info, caps) : -1;
  if (r == 0 && caps) {
    fprintf(stderr, "ee-vkfix: caps2 %ux%u\n", caps->surfaceCapabilities.currentExtent.width,
            caps->surfaceCapabilities.currentExtent.height);
    fflush(stderr);
    fix_caps(&caps->surfaceCapabilities);
    fix_exclusive_out(caps->pNext);
  }
  return r;
}

VkResult vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *info,
                              const VkAllocationCallbacks *alloc, VkSwapchainKHR *swapchain) {
  VkResult (*real)(VkDevice, const VkSwapchainCreateInfoKHR *, const VkAllocationCallbacks *,
                   VkSwapchainKHR *) = real_sym("vkCreateSwapchainKHR");
  VkSwapchainCreateInfoKHR local;
  VkSurfaceFullScreenExclusiveInfoEXT disallowed;
  if (!real)
    return -1;
  force_metal_layers();
  local = *info;
  if (local.imageExtent.width < 320 || local.imageExtent.height < 240) {
    fprintf(stderr, "ee-vkfix: swapchain %ux%u -> 800x600\n", local.imageExtent.width,
            local.imageExtent.height);
    local.imageExtent.width = 800;
    local.imageExtent.height = 600;
  }
  disallowed.sType = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT;
  disallowed.pNext = local.pNext;
  disallowed.fullScreenExclusive = VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT;
  local.pNext = &disallowed;
  {
    VkResult r = real(device, &local, alloc, swapchain);
    force_metal_layers();
    return r;
  }
}

static int is_hook(const char *name) {
  return name && (!strcmp(name, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR") ||
                  !strcmp(name, "vkGetPhysicalDeviceSurfaceCapabilities2KHR") ||
                  !strcmp(name, "vkCreateSwapchainKHR") || !strcmp(name, "vkGetInstanceProcAddr") ||
                  !strcmp(name, "vkGetDeviceProcAddr") || !strcmp(name, "vk_icdGetInstanceProcAddr"));
}

static PFN_vkVoidFunction hook_of(const char *name);

PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice device, const char *name) {
  PFN_vkVoidFunction (*real)(VkDevice, const char *) = real_sym("vkGetDeviceProcAddr");
  if (name && is_hook(name))
    return hook_of(name);
  return real ? real(device, name) : NULL;
}

PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance, const char *name) {
  PFN_vkVoidFunction (*real)(VkInstance, const char *) = real_sym("vkGetInstanceProcAddr");
  if (name && is_hook(name))
    return hook_of(name);
  return real ? real(instance, name) : NULL;
}

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char *name) {
  PFN_vkVoidFunction (*real)(VkInstance, const char *) = real_sym("vk_icdGetInstanceProcAddr");
  if (name && is_hook(name))
    return hook_of(name);
  return real ? real(instance, name) : NULL;
}

static PFN_vkVoidFunction hook_of(const char *name) {
  if (!strcmp(name, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"))
    return (PFN_vkVoidFunction)vkGetPhysicalDeviceSurfaceCapabilitiesKHR;
  if (!strcmp(name, "vkGetPhysicalDeviceSurfaceCapabilities2KHR"))
    return (PFN_vkVoidFunction)vkGetPhysicalDeviceSurfaceCapabilities2KHR;
  if (!strcmp(name, "vkCreateSwapchainKHR"))
    return (PFN_vkVoidFunction)vkCreateSwapchainKHR;
  if (!strcmp(name, "vkGetDeviceProcAddr"))
    return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
  if (!strcmp(name, "vkGetInstanceProcAddr"))
    return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
  if (!strcmp(name, "vk_icdGetInstanceProcAddr"))
    return (PFN_vkVoidFunction)vk_icdGetInstanceProcAddr;
  return NULL;
}
