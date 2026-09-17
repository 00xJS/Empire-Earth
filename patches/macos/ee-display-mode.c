/* List or switch the main display to 800x600 when macOS offers that mode.
 * Does not create fake modes (RDM / BetterDisplay do that).
 */
#include <CoreGraphics/CoreGraphics.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
  int list_only = 0;
  int want_w = 800;
  int want_h = 600;
  CGDirectDisplayID display;
  CFArrayRef modes;
  CFIndex n, i;
  CGDisplayModeRef found = NULL;

  if (argc > 1 && strcmp(argv[1], "--list") == 0)
    list_only = 1;

  display = CGMainDisplayID();
  modes = CGDisplayCopyAllDisplayModes(display, NULL);
  if (!modes) {
    fprintf(stderr, "ee-display-mode: no display modes\n");
    return 1;
  }
  n = CFArrayGetCount(modes);
  for (i = 0; i < n; i++) {
    CGDisplayModeRef mode = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
    size_t w = CGDisplayModeGetWidth(mode);
    size_t h = CGDisplayModeGetHeight(mode);
    printf("%zux%zu\n", w, h);
    if (!found && (int)w == want_w && (int)h == want_h)
      found = mode;
  }
  if (list_only) {
    CFRelease(modes);
    return found ? 0 : 2;
  }
  if (!found) {
    fprintf(stderr,
            "ee-display-mode: %dx%d is not an offered mode. "
            "RDM or BetterDisplay can add a fake 800x600 if you want that workaround.\n",
            want_w, want_h);
    CFRelease(modes);
    return 2;
  }
  if (CGDisplaySetDisplayMode(display, found, NULL) != kCGErrorSuccess) {
    fprintf(stderr, "ee-display-mode: switch to %dx%d failed\n", want_w, want_h);
    CFRelease(modes);
    return 1;
  }
  fprintf(stderr, "ee-display-mode: switched main display to %dx%d\n", want_w, want_h);
  CFRelease(modes);
  return 0;
}
