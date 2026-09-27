/* Models drawn per frame (diagnostics, EE_DRAWSTATS=1).
 *
 * In huge crowds some units vanish for single frames -- mostly the nearest,
 * which the game draws last (27 Sep 2026).  Count the renderer's
 * DX7Rasterizer::Draw(GEModel*) calls per frame (EndScene closes a frame) and
 * log, every 300 frames, the frame rate, the least/most/average per frame, the
 * last 24 frames' counts in order, and which call sites in the game made the
 * calls.  A count
 * pinned at one value in every busy frame would be a limit; one that swings
 * while the camera stands still, a race. */
typedef long(__attribute__((thiscall)) * ds_draw_fn)(void *self, void *model, DWORD flags, float lod, DWORD b);
typedef long(__attribute__((thiscall)) * ds_end_fn)(void *self, DWORD k);
static ds_draw_fn g_ds_draw;
static ds_end_fn g_ds_end;
static LONG g_ds_n, g_ds_min = 0x7fffffff, g_ds_max, g_ds_frames;
static LONGLONG g_ds_sum;
static LONG g_ds_last[24], g_ds_lastn;
static LARGE_INTEGER g_ds_t0;
static struct {
  void *ra;
  LONG n;
} g_ds_site[8];

static long __attribute__((thiscall)) ds_draw(void *self, void *model, DWORD flags, float lod, DWORD b) {
  void *ra = __builtin_return_address(0);
  int i;
  g_ds_n++; /* the render thread only */
  for (i = 0; i < 8; i++) {
    if (g_ds_site[i].ra == ra) {
      g_ds_site[i].n++;
      break;
    }
    if (!g_ds_site[i].ra) {
      g_ds_site[i].ra = ra;
      g_ds_site[i].n = 1;
      break;
    }
  }
  return g_ds_draw(self, model, flags, lod, b);
}

static long __attribute__((thiscall)) ds_end(void *self, DWORD k) {
  LONG n = g_ds_n;
  int i;
  g_ds_n = 0;
  if (n) {
    if (n < g_ds_min)
      g_ds_min = n;
    if (n > g_ds_max)
      g_ds_max = n;
    g_ds_sum += n;
    g_ds_last[g_ds_lastn++ % 24] = n;
    if (++g_ds_frames >= 300) {
      char seq[24 * 7 + 1], sites[8 * 24 + 1];
      int p = 0, q = 0;
      for (i = 0; i < 24; i++)
        p += wsprintfA(seq + p, " %ld", g_ds_last[(g_ds_lastn + i) % 24]);
      for (i = 0; i < 8 && g_ds_site[i].ra; i++) {
        q += wsprintfA(sites + q, " %p:%ld", g_ds_site[i].ra, g_ds_site[i].n);
        g_ds_site[i].n = 0;
      }
      sites[q] = 0;
      {
        LARGE_INTEGER t, f;
        long fps10 = 0;
        QueryPerformanceCounter(&t);
        QueryPerformanceFrequency(&f);
        if (g_ds_t0.QuadPart && t.QuadPart > g_ds_t0.QuadPart)
          fps10 = (long)(g_ds_frames * 10 * f.QuadPart / (t.QuadPart - g_ds_t0.QuadPart));
        g_ds_t0 = t;
        ee_log("draws: %ld frames at %ld.%ld fps, models per frame %ld..%ld (avg %ld); last 24:%s; from%s", g_ds_frames,
               fps10 / 10, fps10 % 10, g_ds_min, g_ds_max, (long)(g_ds_sum / g_ds_frames), seq, sites);
      }
      g_ds_frames = 0;
      g_ds_sum = 0;
      g_ds_min = 0x7fffffff;
      g_ds_max = 0;
    }
  }
  return g_ds_end(self, k);
}

static void ds_install(HMODULE mod) {
  static const unsigned char draw_head[6] = {0x55, 0x8b, 0xec, 0x56, 0x8b, 0xf1};
  static const unsigned char end_head[6] = {0x8b, 0x44, 0x24, 0x04, 0x53, 0x56};
  unsigned char *d, *e;
  void *td, *te;
  char b[8], path[MAX_PATH];
  unsigned i;
  if (!(GetEnvironmentVariableA("EE_DRAWSTATS", b, sizeof b) > 0 && b[0] == '1'))
    return;
  /* only the hardware T&L renderer, the one the game draws with: the two
   * renderers would otherwise share these trampolines */
  GetModuleFileNameA(mod, path, sizeof path);
  for (i = 0; path[i]; i++)
    if (path[i] >= 'A' && path[i] <= 'Z')
      path[i] = (char)(path[i] - 'A' + 'a');
  if (!strstr(path, "tnl"))
    return;
  d = (unsigned char *)GetProcAddress(mod, "?Draw@DX7Rasterizer@@UAEJPAVGEModel@@KM_N@Z");
  e = (unsigned char *)GetProcAddress(mod, "?EndScene@DX7Rasterizer@@UAEJK@Z");
  /* already ours (the game loads its renderers more than once), or not the code */
  if (!d || !e || IsBadReadPtr(d, 6) || IsBadReadPtr(e, 6) || memcmp(d, draw_head, 6) || memcmp(e, end_head, 6))
    return;
  td = ls_hook(d, draw_head, 6, (void *)ds_draw);
  te = td ? ls_hook(e, end_head, 6, (void *)ds_end) : NULL;
  if (td)
    g_ds_draw = (ds_draw_fn)td;
  if (te)
    g_ds_end = (ds_end_fn)te;
  ee_log("draws: counting models per frame (module %p)%s", (void *)mod, td && te ? "" : " -- hook failed");
}
