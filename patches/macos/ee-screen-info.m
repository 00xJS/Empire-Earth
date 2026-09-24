/*
 * Print the main display's size in points, the height of the area a camera
 * notch/menu bar keeps unsafe at its top, and the height of the Mac's own
 * display mode that ends below the notch (0 if there is none):
 *   "<width> <height> <top> <below-notch height>"
 * e.g. a 14" MacBook Pro at its default scaling prints "1512 982 32 945".
 * launch.sh sizes Wine's virtual desktop and the game's resolution from this.
 * Wine's emulated display modes include the Mac's own, so the below-notch mode
 * is one the game can switch to.
 */
#import <AppKit/AppKit.h>

int main(void) {
  @autoreleasepool {
    NSScreen *screen = [NSScreen mainScreen];
    CGFloat top = 0;
    int w, h, below = 0;
    if (!screen)
      return 1;
    if (@available(macOS 12.0, *))
      top = screen.safeAreaInsets.top;
    w = (int)screen.frame.size.width;
    h = (int)screen.frame.size.height;
    if (top > 0) {
      NSDictionary *opts = @{(__bridge NSString *)kCGDisplayShowDuplicateLowResolutionModes : @YES};
      CFArrayRef modes = CGDisplayCopyAllDisplayModes(CGMainDisplayID(), (__bridge CFDictionaryRef)opts);
      for (CFIndex i = 0; modes && i < CFArrayGetCount(modes); i++) {
        CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
        int mw = (int)CGDisplayModeGetWidth(m), mh = (int)CGDisplayModeGetHeight(m);
        if (mw == w && mh < h && mh >= h - (int)top - 16 && mh > below)
          below = mh;
      }
      if (modes)
        CFRelease(modes);
    }
    printf("%d %d %d %d\n", w, h, (int)top, below);
  }
  return 0;
}
