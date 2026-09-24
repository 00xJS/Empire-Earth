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
#import <objc/runtime.h>
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
#define VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_SCALING_CREATE_INFO_EXT 1000275004u
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

/* Full screen (EE_FULLSCREEN=1, set by configure_fullscreen in lib.sh): a black
 * window over the whole display behind the game, and the menu bar and Dock
 * hidden while the game is frontmost.  Wine never touches
 * NSApp.presentationOptions, and the backdrop is not one of Wine's windows, so
 * winemac.drv's own window-level bookkeeping leaves both alone.  A borderless
 * window cannot become key, so clicking the black margin never takes keyboard
 * focus from the game.  The options only apply while the game is the active
 * app: Cmd-Tab away and macOS brings the menu bar and Dock straight back. */
static NSWindow *g_backdrop;

static int fullscreen_wanted(void) {
  static int want = -1;
  if (want < 0) {
    const char *e = getenv("EE_FULLSCREEN");
    want = e && e[0] == '1';
  }
  return want;
}

static __weak NSWindow *g_backdrop_game;

/* ---- Below the camera notch ------------------------------------------------
 * On a MacBook with a notch, launch.sh plays matches in the Mac's own
 * below-notch mode (1512x945 on a 1512x982 display).  Wine keeps a full-screen
 * window the size of the display but centres the shorter game view in it
 * (win32u map_monitor_rect), which leaves half the gap -- and the middle of the
 * game's top bar -- under the notch.  Pin that view to the bottom edge instead,
 * so the whole gap sits under the notch.  Wine re-applies its own frame every
 * time it updates the window, so the frame setters of its view class
 * (WineContentView) are hooked rather than moving the view once.  Only a
 * full-screen, full-width view that holds the game's Metal layer and is short
 * by about the notch moves.  EE_SAFE_TOP (from launch.sh) is the notch height. */
static CGFloat notch_height(void) {
  static CGFloat h = -1;
  if (h < 0) {
    const char *e = getenv("EE_SAFE_TOP");
    h = e ? atof(e) : 0;
    if (h < 0)
      h = 0;
  }
  return h;
}

static NSPoint notch_origin(NSView *view, NSRect frame) {
  NSView *sup = view.superview;
  NSWindow *win = view.window;
  NSRect screen;
  CGFloat gap;
  BOOL metal = NO;
  if (notch_height() <= 0 || !fullscreen_wanted() || !sup || !win || !win.screen || !sup.isFlipped)
    return frame.origin;
  for (NSView *sub in view.subviews)
    if ([sub.layer isKindOfClass:[CAMetalLayer class]]) {
      metal = YES;
      break;
    }
  screen = win.screen.frame;
  gap = sup.bounds.size.height - frame.size.height;
  if (!metal || fabs(win.frame.size.width - screen.size.width) > 0.5 ||
      fabs(win.frame.size.height - screen.size.height) > 0.5 ||
      fabs(frame.size.width - sup.bounds.size.width) > 0.5 || gap < 1 || gap > 2 * notch_height() + 16)
    return frame.origin;
  return NSMakePoint(frame.origin.x, gap); /* the superview is flipped: bottom edge */
}

static void (*real_view_setFrame)(id, SEL, NSRect);
static void (*real_view_setFrameOrigin)(id, SEL, NSPoint);

static void notch_setFrame(id self, SEL sel, NSRect frame) {
  frame.origin = notch_origin(self, frame);
  real_view_setFrame(self, sel, frame);
}

static void notch_setFrameOrigin(id self, SEL sel, NSPoint origin) {
  NSRect frame = [(NSView *)self frame];
  frame.origin = origin;
  real_view_setFrameOrigin(self, sel, notch_origin(self, frame));
}

static void hook_view_method(Class c, SEL sel, IMP imp, IMP *real) {
  Method m = class_getInstanceMethod(c, sel);
  if (!m)
    return;
  *real = method_getImplementation(m);
  /* When Wine's class inherits the method from NSView, add an override to that
   * class alone -- no other view in the process is touched. */
  if (!class_addMethod(c, sel, imp, method_getTypeEncoding(m)))
    *real = method_setImplementation(m, imp);
}

static void pin_below_notch(NSView *metal_view) { /* main thread */
  static int hooked;
  Class wine_view = NSClassFromString(@"WineContentView");
  NSView *client = metal_view.superview;
  NSPoint want;
  if (notch_height() <= 0 || !fullscreen_wanted() || !wine_view)
    return;
  if (!hooked) {
    hooked = 1;
    hook_view_method(wine_view, @selector(setFrame:), (IMP)notch_setFrame, (IMP *)&real_view_setFrame);
    hook_view_method(wine_view, @selector(setFrameOrigin:), (IMP)notch_setFrameOrigin,
                     (IMP *)&real_view_setFrameOrigin);
    fprintf(stderr, "ee-vkfix: below-notch game views pinned to the bottom edge (notch %g)\n", notch_height());
    fflush(stderr);
  }
  if (!client || !real_view_setFrameOrigin || ![client isKindOfClass:wine_view])
    return;
  want = notch_origin(client, client.frame);
  if (!NSEqualPoints(want, client.frame.origin)) {
    fprintf(stderr, "ee-vkfix: game view %gx%g moved from y=%g to y=%g (below the notch)\n", client.frame.size.width,
            client.frame.size.height, client.frame.origin.y, want.y);
    fflush(stderr);
    [client setFrameOrigin:client.frame.origin];
    /* The strip the view leaves shows the window's own background -- dark
     * grey in dark mode, a visible line just under the notch. */
    client.window.backgroundColor = [NSColor blackColor];
  }
}

static void keep_fullscreen(NSWindow *game) {
  NSScreen *screen = game.screen ? game.screen : [NSScreen mainScreen];
  if (!screen)
    return;
  g_backdrop_game = game;
  if (!g_backdrop) {
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    g_backdrop = [[NSWindow alloc] initWithContentRect:screen.frame
                                             styleMask:NSWindowStyleMaskBorderless
                                               backing:NSBackingStoreBuffered
                                                 defer:NO];
    g_backdrop.backgroundColor = [NSColor blackColor];
    g_backdrop.opaque = YES;
    g_backdrop.hasShadow = NO;
    g_backdrop.releasedWhenClosed = NO;
    g_backdrop.animationBehavior = NSWindowAnimationBehaviorNone;
    @try {
      [NSApp setPresentationOptions:NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar];
    } @catch (NSException *e) {
      fprintf(stderr, "ee-vkfix: could not hide the menu bar: %s\n", e.reason.UTF8String);
    }
    /* Only while the game is in front: behind another app, a full-screen black
     * window would hide every window that app is not showing on top of it. */
    [nc addObserverForName:NSApplicationDidResignActiveNotification object:nil
                     queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification *note) {
                  (void)note;
                  [g_backdrop orderOut:nil];
                }];
    [nc addObserverForName:NSApplicationDidBecomeActiveNotification object:nil
                     queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification *note) {
                  NSWindow *g = g_backdrop_game;
                  (void)note;
                  if (g && g.isVisible) {
                    [g_backdrop orderWindow:NSWindowBelow relativeTo:g.windowNumber];
                    /* Coming back to Wine must hand the game the keyboard focus:
                     * Wine only tells the game it is active again (ending its
                     * pause-while-inactive loop) when the game window becomes
                     * key, and activating the app does not always do that --
                     * on 22 Sep 2026 the game sat paused on a black screen
                     * while Wine was the frontmost app. */
                    if (!g.isKeyWindow && g.canBecomeKeyWindow)
                      [g makeKeyWindow];
                  }
                }];
    fprintf(stderr, "ee-vkfix: full screen -- black backdrop %gx%g, menu bar and Dock hidden\n",
            screen.frame.size.width, screen.frame.size.height);
    fflush(stderr);
  }
  if (!NSEqualRects(g_backdrop.frame, screen.frame))
    [g_backdrop setFrame:screen.frame display:NO];
  if (NSApp.isActive)
    [g_backdrop orderWindow:NSWindowBelow relativeTo:game.windowNumber];
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
            pin_below_notch(view);
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
            /* The drawable is the swapchain image size; MoltenVK sets it when the
             * swapchain is created.  Leave a healthy one alone.  Under Wine's
             * emulated display modes the game renders 1024x768 into a view the
             * size of the scaled-up screen area and winevulkan asks MoltenVK to
             * stretch the image to it (VkSwapchainPresentScalingCreateInfoEXT);
             * forcing the drawable to the view's size made MoltenVK copy the
             * frame 1:1 into the view's top-left corner (22 Sep 2026).  Only a
             * degenerate drawable -- the 16x16 plugin-probe windows -- is reset. */
            if (metal.drawableSize.width < 320 || metal.drawableSize.height < 240)
              metal.drawableSize = CGSizeMake(host.width, host.height);
            {
              static double last_w, last_h;
              if (host.width != last_w || host.height != last_h) {
                fprintf(stderr, "ee-vkfix: CAMetalLayer was %gx%g host %gx%g win %gx%g; drawable %gx%g\n",
                        bounds.width, bounds.height, host.width, host.height, win_size.width,
                        win_size.height, metal.drawableSize.width, metal.drawableSize.height);
                fflush(stderr);
                last_w = host.width;
                last_h = host.height;
              }
            }
          }
          if (view.subviews.count)
            [stack addObjectsFromArray:view.subviews];
        }
        /* Asynchronously: this block runs on the main thread while the game's
         * thread waits for it (dispatch_sync below), often from inside the
         * game's CreateDevice.  Reordering windows, changing presentation
         * options or activating the app can make Wine's main thread wait on
         * that same game thread -- never do it while the game is blocked here. */
        if (has_metal && win_size.width >= 700 && win_size.height >= 500 && fullscreen_wanted()) {
          NSWindow *game = window;
          dispatch_async(dispatch_get_main_queue(), ^{
            keep_fullscreen(game);
          });
        }
        if (has_metal && !floated && win_size.width >= 700 && win_size.height >= 500) {
          const char *want_float = getenv("EE_VKFIX_FLOAT");
          floated = 1;
          /* Let the game's window follow the user between Spaces instead of
           * stranding itself on the Space it happened to be created on.  Two
           * reasons: you cannot click a window you cannot see, and -- worse --
           * a window on an inactive Space is occluded, which makes DXVK report
           * device-lost and D7VK return DDERR_SURFACELOST from every Flip.
           * That is the same failure as the minimized window, just triggered by
           * occlusion rather than by SW_MINIMIZE. */
          window.collectionBehavior |= NSWindowCollectionBehaviorMoveToActiveSpace;
          /* NSFloatingWindowLevel pins the game above every other application,
           * which is half of why the window could not be escaped.  It was added
           * when the window kept being lost; the ddraw-side watchdog covers that
           * now, so this is opt-in. */
          if (want_float && want_float[0] == '1')
            [window setLevel:NSFloatingWindowLevel];
          {
            NSWindow *front = window;
            dispatch_async(dispatch_get_main_queue(), ^{
              [front orderFrontRegardless];
              [NSApp activateIgnoringOtherApps:YES];
            });
          }
          fprintf(stderr, "ee-vkfix: bringing Metal window to front frame=%gx%g\n", win_size.width,
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
    /* Any size down to 1x1: winevulkan raises the host swapchain to at least
     * minImageExtent, and MoltenVK reports the whole view there.  Under Wine's
     * emulated display modes the game's swapchain (1024x768 for the menu) is
     * smaller than its scaled-up view, and being raised to the view's size
     * left DXVK drawing into the top-left 1024x768 of bigger images.  At the
     * game's size the layer's contentsGravity scales it up instead. */
    caps->minImageExtent.width = 1;
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
    static uint32_t last_w, last_h, last_mw, last_mh;
    if (caps->currentExtent.width != last_w || caps->currentExtent.height != last_h ||
        caps->minImageExtent.width != last_mw || caps->minImageExtent.height != last_mh) {
      fprintf(stderr, "ee-vkfix: caps %ux%u (min %ux%u)\n", caps->currentExtent.width, caps->currentExtent.height,
              caps->minImageExtent.width, caps->minImageExtent.height);
      fflush(stderr);
      last_w = caps->currentExtent.width;
      last_h = caps->currentExtent.height;
      last_mw = caps->minImageExtent.width;
      last_mh = caps->minImageExtent.height;
    }
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
    int scaled = 0;
    for (const VkBaseOutStructure *n = info->pNext; n; n = n->pNext)
      if (n->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_SCALING_CREATE_INFO_EXT)
        scaled = 1;
    fprintf(stderr, "ee-vkfix: swapchain %ux%u created r=%d%s\n", local.imageExtent.width,
            local.imageExtent.height, r, scaled ? " (present scaling requested)" : "");
    fflush(stderr);
    force_metal_layers();
    return r;
  }
}

typedef struct VkQueue_T *VkQueue;
typedef struct {
  VkStructureType sType;
  const void *pNext;
  uint32_t waitSemaphoreCount;
  const void *pWaitSemaphores;
  uint32_t swapchainCount;
  const VkSwapchainKHR *pSwapchains;
  const uint32_t *pImageIndices;
  VkResult *pResults;
} VkPresentInfoKHR;
#define VK_SUBOPTIMAL_KHR 1000001003

/* MoltenVK reports VK_SUBOPTIMAL_KHR whenever a swapchain is smaller than its
 * CAMetalLayer -- which is the whole point when the layer scales the game's
 * 1024x768 menu up to the screen under Wine's emulated display modes.
 * DXVK-Sarek's D3D9 swapchain recreates itself after every present that is not
 * exactly VK_SUCCESS (SynchronizePresent), i.e. every frame.  Pass it on as
 * success: DXVK still recreates on a real window resize from its own client-size
 * check, and errors (OUT_OF_DATE, SURFACE_LOST) pass through untouched. */
VkResult vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *info) {
  VkResult (*real)(VkQueue, const VkPresentInfoKHR *) = real_sym("vkQueuePresentKHR");
  VkResult r = real ? real(queue, info) : -1;
  if (info && info->pResults)
    for (uint32_t i = 0; i < info->swapchainCount; i++)
      if (info->pResults[i] == VK_SUBOPTIMAL_KHR)
        info->pResults[i] = 0;
  if (r == VK_SUBOPTIMAL_KHR) {
    static int logged;
    if (!logged++) {
      fprintf(stderr, "ee-vkfix: present suboptimal (swapchain smaller than its layer, which scales it) -> success\n");
      fflush(stderr);
    }
    r = 0;
  }
  return r;
}

typedef struct VkDeviceMemory_T *VkDeviceMemory;
typedef struct {
  VkStructureType sType;
  const void *pNext;
  uint64_t allocationSize;
  uint32_t memoryTypeIndex;
} VkMemoryAllocateInfo;
typedef struct {
  VkStructureType sType;
  const void *pNext;
  uint32_t handleType;
  void *pHostPointer;
} VkImportMemoryHostPointerInfoEXT;
#define VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT 1000178000u

/* A 32-bit game's memory is mapped read-write-EXECUTE by Wine (no DEP for old
 * executables), including the host memory winevulkan allocates for every
 * host-visible Vulkan allocation of a WoW64 process and MoltenVK imports into
 * Metal (VK_EXT_external_memory_host).  On 22 Sep 2026 gameplay froze for 10-50 s
 * at a time with DXVK's render thread stuck on one `rep stosd` into such a chunk
 * (vmmap: rwx/rwx SM=SHM) for 28+ s while every other thread sat in the Rosetta
 * runtime and the process took ~80,000 page faults a second -- Rosetta treats a
 * writable+executable page as possible code.  Nothing ever executes from Vulkan
 * memory, so drop the execute bit before MoltenVK sees it.  EE_VKFIX_NOEXEC=0
 * turns this off. */
VkResult vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo *info, const VkAllocationCallbacks *alloc,
                          VkDeviceMemory *memory) {
  VkResult (*real)(VkDevice, const VkMemoryAllocateInfo *, const VkAllocationCallbacks *, VkDeviceMemory *) =
      real_sym("vkAllocateMemory");
  static int want = -1;
  if (want < 0) {
    const char *e = getenv("EE_VKFIX_NOEXEC");
    want = !(e && e[0] == '0');
  }
  if (want && info) {
    for (const VkBaseOutStructure *n = info->pNext; n; n = n->pNext) {
      if (n->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT) {
        const VkImportMemoryHostPointerInfoEXT *imp = (const VkImportMemoryHostPointerInfoEXT *)n;
        uintptr_t page = (uintptr_t)getpagesize();
        uintptr_t start = (uintptr_t)imp->pHostPointer & ~(page - 1);
        uintptr_t end = ((uintptr_t)imp->pHostPointer + (uintptr_t)info->allocationSize + page - 1) & ~(page - 1);
        int rc = imp->pHostPointer ? mprotect((void *)start, end - start, PROT_READ | PROT_WRITE) : -1;
        static int logged;
        if (logged++ < 5) {
          fprintf(stderr, "ee-vkfix: Vulkan host memory %p+%llu imported read-write, no exec (mprotect %d)\n",
                  imp->pHostPointer, (unsigned long long)info->allocationSize, rc);
          fflush(stderr);
        }
      }
    }
  }
  return real ? real(device, info, alloc, memory) : -1;
}

static int is_hook(const char *name) {
  return name && (!strcmp(name, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR") ||
                  !strcmp(name, "vkGetPhysicalDeviceSurfaceCapabilities2KHR") ||
                  !strcmp(name, "vkCreateSwapchainKHR") || !strcmp(name, "vkQueuePresentKHR") ||
                  !strcmp(name, "vkAllocateMemory") ||
                  !strcmp(name, "vkGetInstanceProcAddr") || !strcmp(name, "vkGetDeviceProcAddr") ||
                  !strcmp(name, "vk_icdGetInstanceProcAddr"));
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
  if (!strcmp(name, "vkQueuePresentKHR"))
    return (PFN_vkVoidFunction)vkQueuePresentKHR;
  if (!strcmp(name, "vkAllocateMemory"))
    return (PFN_vkVoidFunction)vkAllocateMemory;
  if (!strcmp(name, "vkGetDeviceProcAddr"))
    return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
  if (!strcmp(name, "vkGetInstanceProcAddr"))
    return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
  if (!strcmp(name, "vk_icdGetInstanceProcAddr"))
    return (PFN_vkVoidFunction)vk_icdGetInstanceProcAddr;
  return NULL;
}
