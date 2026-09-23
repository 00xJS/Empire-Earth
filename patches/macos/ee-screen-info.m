/*
 * Print the main display's size in points and the height of the area a
 * camera notch/menu bar keeps unsafe at its top:  "<width> <height> <top>".
 * e.g. a 14" MacBook Pro at its default scaling prints "1512 982 32".
 * launch.sh sizes Wine's virtual desktop and the game's resolution from this.
 */
#import <AppKit/AppKit.h>

int main(void) {
  @autoreleasepool {
    NSScreen *screen = [NSScreen mainScreen];
    CGFloat top = 0;
    if (!screen)
      return 1;
    if (@available(macOS 12.0, *))
      top = screen.safeAreaInsets.top;
    printf("%d %d %d\n", (int)screen.frame.size.width, (int)screen.frame.size.height, (int)top);
  }
  return 0;
}
