/* Exit 0 if a visible Wine window looks like it left the 640x260 GDI splash.
 * Exit 1 if we are still on that banner (or there is no on-screen game view).
 * Does not talk to System Events.
 */
#include <CoreGraphics/CoreGraphics.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static int is_wine_owner(const char *owner) {
  return strcasecmp(owner, "wine") == 0 || strcasecmp(owner, "Wine") == 0;
}

static int is_splash(double w, double h) {
  return w >= 630 && w <= 650 && h >= 250 && h <= 270;
}

int main(void) {
  CFArrayRef wins = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
  int saw_splash = 0;
  int saw_game = 0;
  if (!wins)
    return 1;
  CFIndex n = CFArrayGetCount(wins);
  for (CFIndex i = 0; i < n; i++) {
    CFDictionaryRef d = CFArrayGetValueAtIndex(wins, i);
    CFStringRef owner_s = CFDictionaryGetValue(d, kCGWindowOwnerName);
    char owner[256] = {0};
    CFDictionaryRef bounds;
    double w = 0, h = 0;
    if (owner_s)
      CFStringGetCString(owner_s, owner, sizeof owner, kCFStringEncodingUTF8);
    if (!is_wine_owner(owner))
      continue;
    bounds = CFDictionaryGetValue(d, kCGWindowBounds);
    if (bounds) {
      CFNumberRef nw = CFDictionaryGetValue(bounds, CFSTR("Width"));
      CFNumberRef nh = CFDictionaryGetValue(bounds, CFSTR("Height"));
      if (nw)
        CFNumberGetValue(nw, kCFNumberDoubleType, &w);
      if (nh)
        CFNumberGetValue(nh, kCFNumberDoubleType, &h);
    }
    {
      int64_t wid = 0;
      CFNumberRef idn = CFDictionaryGetValue(d, kCGWindowNumber);
      if (idn)
        CFNumberGetValue(idn, kCFNumberSInt64Type, &wid);
      fprintf(stderr, "ee-splash-probe: wine id=%lld %gx%g\n", (long long)wid, w, h);
    }
    if (is_splash(w, h)) {
      saw_splash = 1;
      continue;
    }
    if (w >= 320 && h >= 240) {
      saw_game = 1;
      fprintf(stderr, "ee-splash-probe: on-screen %gx%g\n", w, h);
    }
  }
  CFRelease(wins);
  if (saw_game && saw_splash) {
    fprintf(stderr, "ee-splash-probe: 3D window up but splash still visible\n");
    return 1;
  }
  if (saw_game) {
    fprintf(stderr, "ee-splash-probe: past splash\n");
    return 0;
  }
  fprintf(stderr, "ee-splash-probe: %s\n", saw_splash ? "still on 640x260 splash" : "no on-screen game window");
  return 1;
}
