/* One-shot: move Empire Earth's 800x600 3D window on-screen and hide the splash. */
#include <windows.h>
#include <stdio.h>

static BOOL CALLBACK enum_proc(HWND hwnd, LPARAM lp) {
  RECT cr, wr;
  int w, h;
  char title[256];
  (void)lp;
  GetClientRect(hwnd, &cr);
  GetWindowRect(hwnd, &wr);
  w = cr.right - cr.left;
  h = cr.bottom - cr.top;
  GetWindowTextA(hwnd, title, sizeof title);
  if (strstr(title, "IME"))
    return TRUE;
  if (w == 640 && h >= 250 && h <= 270) {
    fprintf(stderr, "ee-nudge: hide splash %p\n", (void *)hwnd);
    ShowWindow(hwnd, SW_HIDE);
    return TRUE;
  }
  if (w >= 320 && h >= 240 && w <= 1280 && h <= 1024) {
    fprintf(stderr, "ee-nudge: %p %dx%d \"%s\" frame %ld,%ld -> 80,80\n", (void *)hwnd, w, h, title,
            (long)wr.left, (long)wr.top);
    ShowWindow(hwnd, SW_SHOW);
    SetWindowPos(hwnd, HWND_TOP, 80, 80, 800, 600, SWP_SHOWWINDOW);
  }
  return TRUE;
}

int main(void) {
  EnumWindows(enum_proc, 0);
  return 0;
}
