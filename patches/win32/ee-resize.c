/* Grow tiny Empire Earth 3D HWNDs to 800x600. Skips the 640x260 GDI splash.
 * Does not modify game binaries. Run alongside Empire Earth.exe in the same prefix.
 */
#include <windows.h>
#include <stdio.h>

static BOOL CALLBACK enum_proc(HWND hwnd, LPARAM lp) {
  RECT r;
  int w, h;
  char title[256];
  DWORD pid = 0;
  (void)lp;
  GetWindowThreadProcessId(hwnd, &pid);
  if (!pid)
    return TRUE;
  GetClientRect(hwnd, &r);
  w = r.right - r.left;
  h = r.bottom - r.top;
  GetWindowTextA(hwnd, title, sizeof title);
  if (strstr(title, "IME") || strstr(title, "MSCTFIME"))
    return TRUE;
  if (w <= 0 || h <= 0)
    return TRUE;
  if (h <= 32)
    return TRUE;
  if (w == 640 && h >= 250 && h <= 270)
    return TRUE;
  if (w < 320 || h < 240) {
    fprintf(stderr, "ee-resize: hwnd %p %dx%d \"%s\" -> 800x600\n", (void *)hwnd, w, h, title);
    fflush(stderr);
    ShowWindow(hwnd, SW_SHOW);
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 800, 600, SWP_NOMOVE | SWP_SHOWWINDOW);
  }
  return TRUE;
}

int main(void) {
  DEVMODEA mode;
  int i;
  memset(&mode, 0, sizeof mode);
  mode.dmSize = sizeof mode;
  mode.dmPelsWidth = 800;
  mode.dmPelsHeight = 600;
  mode.dmBitsPerPel = 32;
  mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
  if (ChangeDisplaySettingsA(&mode, CDS_FULLSCREEN) == DISP_CHANGE_SUCCESSFUL)
    fprintf(stderr, "ee-resize: ChangeDisplaySettings 800x600 ok\n");
  else
    fprintf(stderr, "ee-resize: ChangeDisplaySettings 800x600 not available (expected on Apple Silicon)\n");
  fflush(stderr);
  for (i = 0; i < 600; i++) {
    EnumWindows(enum_proc, 0);
    Sleep(200);
  }
  return 0;
}
