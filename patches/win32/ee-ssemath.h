/* ---- SSE maths: Low-Level Engine.dll's hot x87 functions, rewritten --------
 *
 * Empire Earth (2001, Visual C++ 6) does all its maths on the x87 FPU. Apple
 * Silicon has no x87; Rosetta 2 emulates it in software, so on this Mac a
 * 3x4 transform of one point costs ~570 ns in x87 and ~2.5 ns in SSE2 (Wine,
 * M2 Pro, 25 Sep 2026 -- diagnostics/fpbench).  The engine's transforms,
 * plane tests and cosine table were ~21% of the game's main thread in a big
 * match, the rest of its maths another ~10%.
 *
 * Each replacement redoes the original's x87 sequence exactly (see
 * ee-ssemath-body.h), in double for x87 at its Windows-default 53-bit
 * precision or in float for 24-bit (picked per call from the caller's x87
 * control word, as the original would round), and ee_ssemath_install() jumps the
 * original's first instruction to it -- after checking that the function's
 * bytes are the ones that were reversed (Art of Conquest ships its own DLL).
 * diagnostics/ssemath-test.exe compares every replacement against the real
 * DLL on random input, bit for bit.  EE_SSE_MATH=0 turns it off. */
#include <string.h>

#define SSEM_FN __attribute__((target("sse2,fpmath=sse"), noinline, used)) /* used: keep the standard ABI for float args */
#define SSEM_THIS __attribute__((thiscall))

static struct {
  float cos_lo, cos_step, cos_hi, cos_scale, half_pi, terrain_k;
  const float *cos_table;
  /* UMArcCos / UMArcTan2 */
  float ac_lim, ac_scale, ac_c3, ac_c4, ac_c5, ac_clamp, ac_pi, ac_nhalf_pi;
  const float *ac_table;
  double(__cdecl *crt_sqrt)(double); /* the DLL's msvcrt sqrt, through its import table */
} g_ssem;

/* UMCos statistics (on with EE_LOCK_STATS=1): its range reduction steps the
 * angle by 2 pi at a time, so huge angles cost one loop step per turn.  Racy
 * counters -- statistics only; nothing here touches the arithmetic. */
static struct {
  int on;
  volatile unsigned calls, steps, big; /* big: calls over 64 steps */
  float max;                           /* largest |angle| */
  struct { void *ra; unsigned n; } site[8]; /* the game's call sites of big calls */
} g_ssem_st;
static void ssem_st_site(void *ra) {
  int i;
  for (i = 0; i < 8; i++)
    if (g_ssem_st.site[i].ra == ra || !g_ssem_st.site[i].ra) {
      g_ssem_st.site[i].ra = ra;
      g_ssem_st.site[i].n++;
      return;
    }
}

/* The calling thread's x87 control word: precision in bits 8-9 (00 = 24-bit,
 * 10 = 53, 11 = 64), rounding in 10-11.  0.4 ns under Rosetta, so every call
 * checks it and runs the variant that matches the original exactly (64-bit
 * precision gets the double variant, the closest). */
static inline unsigned short ssem_cw(void) {
  unsigned short cw;
  __asm__ volatile("fnstcw %0" : "=m"(cw));
  return cw;
}

/* fistp of a float under the thread's x87 rounding mode. */
SSEM_FN static int ssem_fistp(float t) {
  int i;
  switch ((ssem_cw() >> 10) & 3) {
  case 0: __asm__("cvtss2si %1, %0" : "=r"(i) : "x"(t)); return i; /* MXCSR default: nearest-even */
  case 3: __asm__("cvttss2si %1, %0" : "=r"(i) : "x"(t)); return i; /* chop */
  case 1: __asm__("cvttss2si %1, %0" : "=r"(i) : "x"(t)); return (float)i > t ? i - 1 : i; /* down */
  default: __asm__("cvttss2si %1, %0" : "=r"(i) : "x"(t)); return (float)i < t ? i + 1 : i; /* up */
  }
}

/* A float copied through an x87 load and store: bit for bit, except that a
 * signalling NaN comes out quiet. */
static inline void ssem_copyq(void *dst, const void *src) {
  unsigned u;
  memcpy(&u, src, 4);
  if ((u & 0x7f800000u) == 0x7f800000u && (u & 0x003fffffu) && !(u & 0x00400000u))
    u |= 0x00400000u;
  memcpy(dst, &u, 4);
}

#define T double
#define SSEM(n) ssem_##n##_d
#include "ee-ssemath-body.h"
#undef T
#undef SSEM
#define T float
#define SSEM(n) ssem_##n##_f
#include "ee-ssemath-body.h"
#undef T
#undef SSEM

#define SSEM_PC24() ((ssem_cw() & 0x300) == 0)
static void SSEM_THIS ssem_apply_p3(const float *m, const float *in, float *out) {
  if (SSEM_PC24()) ssem_apply_p3_f(m, in, out); else ssem_apply_p3_d(m, in, out);
}
static void SSEM_THIS ssem_apply_v3(const float *m, const float *in, float *out) {
  if (SSEM_PC24()) ssem_apply_v3_f(m, in, out); else ssem_apply_v3_d(m, in, out);
}
static void SSEM_THIS ssem_apply_p2(const float *m, const float *in, float *out) {
  if (SSEM_PC24()) ssem_apply_p2_f(m, in, out); else ssem_apply_p2_d(m, in, out);
}
static void SSEM_THIS ssem_apply_xf(const float *a, const float *b, float *out) {
  if (SSEM_PC24()) ssem_apply_xf_f(a, b, out); else ssem_apply_xf_d(a, b, out);
}
static void SSEM_THIS ssem_cross(const float *a, const float *b, float *out) {
  if (SSEM_PC24()) ssem_cross_f(a, b, out); else ssem_cross_d(a, b, out);
}
static double SSEM_THIS ssem_dist2(const float *a, const float *b) {
  return SSEM_PC24() ? (double)ssem_dist2_f(a, b) : ssem_dist2_d(a, b);
}
static int SSEM_THIS ssem_above(const float *pl, const float *pt) {
  return SSEM_PC24() ? ssem_above_f(pl, pt) : ssem_above_d(pl, pt);
}
static int SSEM_THIS ssem_below(const float *pl, const float *pt) {
  return SSEM_PC24() ? ssem_below_f(pl, pt) : ssem_below_d(pl, pt);
}
static float ssem_umcos(float a) {
  const unsigned big = g_ssem_st.big;
  const float r = SSEM_PC24() ? ssem_umcos_f(a) : ssem_umcos_d(a);
  if (g_ssem_st.on && g_ssem_st.big != big)
    ssem_st_site(__builtin_return_address(0));
  return r;
}
static void SSEM_THIS ssem_bbox_dims(float *b) {
  if (SSEM_PC24()) ssem_bbox_dims_f(b); else ssem_bbox_dims_d(b);
}
static void SSEM_THIS ssem_ypr(float *m, float a, float b, float c) {
  const unsigned big = g_ssem_st.big;
  if (SSEM_PC24()) ssem_ypr_f(m, a, b, c); else ssem_ypr_d(m, a, b, c);
  if (g_ssem_st.on && g_ssem_st.big != big)
    ssem_st_site(__builtin_return_address(0));
}
static int SSEM_THIS ssem_line_plane(const float *L, const float *pl, float *t) {
  return SSEM_PC24() ? ssem_line_plane_f(L, pl, t) : ssem_line_plane_d(L, pl, t);
}
static void SSEM_THIS ssem_line_dir(float *L, const float *q) {
  if (SSEM_PC24()) ssem_line_dir_f(L, q); else ssem_line_dir_d(L, q);
}
static double ssem_umacos(float x) { return SSEM_PC24() ? (double)ssem_umacos_f(x) : ssem_umacos_d(x); }
static double ssem_umatan2(float a, float b) { return SSEM_PC24() ? (double)ssem_umatan2_f(a, b) : ssem_umatan2_d(a, b); }
static int SSEM_THIS ssem_line_sphere(const float *L, const float *P, float r, float *t) {
  return SSEM_PC24() ? ssem_line_sphere_f(L, P, r, t) : ssem_line_sphere_d(L, P, r, t);
}
static void SSEM_THIS ssem_norm_cam(const char *vp, const float *in, float *out, float *w) {
  if (SSEM_PC24()) ssem_norm_cam_f(vp, in, out, w); else ssem_norm_cam_d(vp, in, out, w);
}
static void SSEM_THIS ssem_norm_ortho(const char *vp, const float *in, float *out, float *w) {
  if (SSEM_PC24()) ssem_norm_ortho_f(vp, in, out, w); else ssem_norm_ortho_d(vp, in, out, w);
}
/* thiscall on the rasterizer, which it does not use */
static double SSEM_THIS ssem_pixel_size(void *self, const char *model, const char *vp, const float *xf, float scale, unsigned ortho) {
  (void)self;
  return SSEM_PC24() ? (double)ssem_pixel_size_f(model, vp, xf, scale, ortho) : ssem_pixel_size_d(model, vp, xf, scale, ortho);
}
static void SSEM_THIS ssem_prep_xf(char *model) {
  if (SSEM_PC24()) ssem_prep_xf_f(model); else ssem_prep_xf_d(model);
}
static void SSEM_THIS ssem_persp_vis(void *self, unsigned char *model, float scale, const char *vp, const float *xf,
                                    unsigned char *visible, unsigned char *clipped) {
  (void)self;
  if (SSEM_PC24()) ssem_persp_vis_f(model, scale, vp, xf, visible, clipped);
  else ssem_persp_vis_d(model, scale, vp, xf, visible, clipped);
}
static double SSEM_THIS ssem_meshz(const char *mesh, float x, float y) {
  return SSEM_PC24() ? (double)ssem_meshz_f(mesh, x, y) : ssem_meshz_d(mesh, x, y);
}
/* The two smoothing loops are thiscall with three stack arguments; `this' (the
 * rasterizer) is not used. */
static void SSEM_THIS ssem_smooth_std(void *self, const char *model, const char *mat, char *out) {
  (void)self;
  if (SSEM_PC24()) ssem_smooth_f(model, mat, out, 0); else ssem_smooth_d(model, mat, out, 0);
}
static void SSEM_THIS ssem_smooth_color(void *self, const char *model, const char *mat, char *out) {
  (void)self;
  if (SSEM_PC24()) ssem_smooth_f(model, mat, out, 1); else ssem_smooth_d(model, mat, out, 1);
}
/* ??0GE3DPlane@@QAE@XZ (rva 0x32c9): point 0,0,0, normal 0,0,1; returns this. */
static void *SSEM_THIS ssem_plane_ctor(float *p) {
  static const float v[6] = {0, 0, 0, 0, 0, 1};
  memcpy(p, v, sizeof v);
  return p;
}
/* ??0GE3DLine@@QAE@XZ (rva 0x1e46): 0,0,0 then 1,0,0; returns this. */
static void *SSEM_THIS ssem_line_ctor(float *p) {
  static const float v[6] = {0, 0, 0, 1, 0, 0};
  memcpy(p, v, sizeof v);
  return p;
}
/* ??0GEBoundingBox@@QAE@XZ (rva 0x3990): min 0,0,0, max 1,1,1, then
 * ComputeDimensions; returns this. */
static void *SSEM_THIS ssem_bbox_ctor(float *b) {
  static const float v[6] = {0, 0, 0, 1, 1, 1};
  memcpy(b, v, sizeof v);
  ssem_bbox_dims(b);
  return b;
}

/* ??0GETransformation@@QAE@XZ  (rva 0x466c8): identity; returns this. */
static void *SSEM_THIS ssem_ctor(float *m) {
  static const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  memcpy(m, id, sizeof id);
  return m;
}

struct ssem_patch {
  const char *name;
  unsigned len, fnv; /* the reversed function: its length and FNV-1a of its bytes */
  void *to;          /* its replacement (a dispatcher on the caller's x87 mode) */
};

#define SSEM_P(nm, len, fnv, fn) {nm, len, fnv, (void *)ssem_##fn}
static const struct ssem_patch g_ssem_patches[] = {
    SSEM_P("?Apply@GETransformation@@QBEXABVGE3DPoint@@AAV2@@Z", 90, 0x12f6ba99, apply_p3),
    SSEM_P("?Apply@GETransformation@@QBEXABVGEVector@@AAV2@@Z", 81, 0x58e4f25c, apply_v3),
    SSEM_P("?Apply@GETransformation@@QBEXABVGE2DPoint@@AAV2@@Z", 47, 0x3dc203b7, apply_p2),
    SSEM_P("?Apply@GETransformation@@QBEXABV1@AAV1@@Z", 473, 0x9675e85a, apply_xf),
    SSEM_P("?CrossProduct@GEVector@@QBEXABV1@AAV1@@Z", 57, 0x82bb4567, cross),
    SSEM_P("?DistanceToSquared@GE3DPoint@@QBEMABV1@@Z", 45, 0x1322e746, dist2),
    SSEM_P("?IsPointOnOrAbovePlane@GE3DPlane@@QBE_NABVGE3DPoint@@@Z", 71, 0xa1b30ba4, above),
    SSEM_P("?IsPointOnOrBelowPlane@GE3DPlane@@QBE_NABVGE3DPoint@@@Z", 71, 0xf05d30e3, below),
    SSEM_P("?UMCos@@YAMM@Z", 101, 0xce1de810, umcos),
    {"??0GETransformation@@QAE@XZ", 82, 0x1a835b38, (void *)ssem_ctor},
    {"??0GE3DPlane@@QAE@XZ", 37, 0xe15509de, (void *)ssem_plane_ctor},
    {"??0GE3DLine@@QAE@XZ", 32, 0x445ed7ea, (void *)ssem_line_ctor},
    {"??0GEBoundingBox@@QAE@XZ", 41, 0x895a38a8, (void *)ssem_bbox_ctor},
    SSEM_P("?ComputeDimensions@GEBoundingBox@@AAEXXZ", 105, 0x46cedc3a, bbox_dims),
    SSEM_P("?SetOrientationYPR@GETransformation@@QAEXMMM@Z", 244, 0xf582eacc, ypr),
    SSEM_P("?Intersects@GE3DLine@@QBE_NABVGE3DPlane@@AAM@Z", 111, 0x0d9d1aad, line_plane),
    SSEM_P("?ComputeDirection@GE3DLine@@AAEXABVGE3DPoint@@@Z", 38, 0xeaab56a4, line_dir),
    SSEM_P("?GetMeshZ@GETerrainMesh@@QBEMMM@Z", 242, 0x458f79b6, meshz),
    SSEM_P("?UMArcCos@@YAMM@Z", 224, 0x88ed17fb, umacos),
    SSEM_P("?UMArcTan2@@YAMMM@Z", 109, 0xb86f9ac9, umatan2),
    SSEM_P("?Intersects@GE3DLine@@QBE_NABVGE3DPoint@@MAAM@Z", 392, 0x304b5817, line_sphere),
    SSEM_P("?NormalizePerspectiveCameraPoint@GEViewport@@QAEXABVGE3DPoint@@AAV2@AAM@Z", 60, 0x3fc6890c, norm_cam),
    SSEM_P("?NormalizeOrthographicCameraPoint@GEViewport@@QAEXABVGE3DPoint@@AAV2@AAM@Z", 59, 0xb873b47a, norm_ortho),
    SSEM_P("?GetPixelSize@GERasterizer@@QAEMPAVGEModel@@PAVGEViewport@@AAVGETransformation@@M_N@Z", 300, 0x9681a4dd, pixel_size),
    SSEM_P("?PrepareTransforms@GEModel@@QAEXXZ", 227, 0x1a566115, prep_xf),
    SSEM_P("?IsPerspectiveModelVisible@GERasterizer@@IAEXPAVGEModel@@MPAVGEViewport@@AAVGETransformation@@AA_N3@Z", 843, 0xc4526dbd, persp_vis),
};
#define SSEM_N ((int)(sizeof g_ssem_patches / sizeof g_ssem_patches[0]))

static unsigned ssem_fnv(const unsigned char *p, unsigned n) {
  unsigned h = 2166136261u;
  while (n--)
    h = (h ^ *p++) * 16777619u;
  return h;
}

/* Reads UMCos's constants and table from the loaded DLL (lle = its base). */
/* Which of the game's direct calls each replacement stands in for: the call at
 * +off in the reversed function must land on `callee' (an export of the same
 * DLL; "sqrt" = an import thunk to msvcrt's sqrt), and the callee must itself
 * be the reversed code, since the SSE version runs its own copy of it. */
#define SSEM_YPR "?SetOrientationYPR@GETransformation@@QAEXMMM@Z"
#define SSEM_COS "?UMCos@@YAMM@Z"
#define SSEM_ACOS "?UMArcCos@@YAMM@Z"
#define SSEM_ATAN2 "?UMArcTan2@@YAMMM@Z"
#define SSEM_LSPH "?Intersects@GE3DLine@@QBE_NABVGE3DPoint@@MAAM@Z"
#define SSEM_PIX "?GetPixelSize@GERasterizer@@QAEMPAVGEModel@@PAVGEViewport@@AAVGETransformation@@M_N@Z"
#define SSEM_PREP "?PrepareTransforms@GEModel@@QAEXXZ"
#define SSEM_VIS "?IsPerspectiveModelVisible@GERasterizer@@IAEXPAVGEModel@@MPAVGEViewport@@AAVGETransformation@@AA_N3@Z"
#define SSEM_AP3 "?Apply@GETransformation@@QBEXABVGE3DPoint@@AAV2@@Z"
#define SSEM_AXF "?Apply@GETransformation@@QBEXABV1@AAV1@@Z"
#define SSEM_NPC "?NormalizePerspectiveCameraPoint@GEViewport@@QAEXABVGE3DPoint@@AAV2@AAM@Z"
#define SSEM_NOC "?NormalizeOrthographicCameraPoint@GEViewport@@QAEXABVGE3DPoint@@AAV2@AAM@Z"
#define SSEM_ABOVE "?IsPointOnOrAbovePlane@GE3DPlane@@QBE_NABVGE3DPoint@@@Z"
#define SSEM_BELOW "?IsPointOnOrBelowPlane@GE3DPlane@@QBE_NABVGE3DPoint@@@Z"
#define SSEM_MAG2 "?GetMagnitudeSquared@GEVector@@QBEMXZ"
static const struct { const char *fn, *callee; unsigned short off; } g_ssem_calls[] = {
    {"??0GEBoundingBox@@QAE@XZ", "?ComputeDimensions@GEBoundingBox@@AAEXXZ", 0x20},
    {SSEM_YPR, SSEM_COS, 0x10}, {SSEM_YPR, SSEM_COS, 0x24}, {SSEM_YPR, SSEM_COS, 0x37},
    {SSEM_YPR, SSEM_COS, 0x4b}, {SSEM_YPR, SSEM_COS, 0x5e}, {SSEM_YPR, SSEM_COS, 0x72},
    {SSEM_ACOS, "sqrt", 0x58}, {SSEM_ACOS, SSEM_ACOS, 0xc8},
    {SSEM_ATAN2, "sqrt", 0x24}, {SSEM_ATAN2, SSEM_ACOS, 0x40}, {SSEM_ATAN2, SSEM_ACOS, 0x48},
    {SSEM_LSPH, SSEM_MAG2, 0x32}, {SSEM_LSPH, "sqrt", 0x107},
    {SSEM_PIX, SSEM_AP3, 0x37}, {SSEM_PIX, SSEM_AP3, 0x4d}, {SSEM_PIX, SSEM_NPC, 0x84},
    {SSEM_PIX, SSEM_NPC, 0xbb}, {SSEM_PIX, SSEM_NOC, 0xda}, {SSEM_PIX, SSEM_NOC, 0xfc},
    {SSEM_PREP, SSEM_AXF, 0x37}, {SSEM_PREP, SSEM_AXF, 0xc0}, {SSEM_PREP, SSEM_PREP, 0xd4},
    {SSEM_VIS, SSEM_AP3, 0x9d}, {SSEM_VIS, SSEM_AP3, 0xb0}, {SSEM_VIS, SSEM_ABOVE, 0x17d},
    {SSEM_VIS, SSEM_ABOVE, 0x1af}, {SSEM_VIS, SSEM_ABOVE, 0x1e4}, {SSEM_VIS, SSEM_ABOVE, 0x219},
    {SSEM_VIS, SSEM_BELOW, 0x292}, {SSEM_VIS, SSEM_BELOW, 0x2c4}, {SSEM_VIS, SSEM_BELOW, 0x2f6},
    {SSEM_VIS, SSEM_BELOW, 0x328},
};
#define SSEM_MAG2_LEN 31 /* not replaced, but the line-sphere test inlines it */

/* FNV-1a of a function's bytes, with every base-relocated dword -- an absolute
 * address, which the loader rewrites (the game loads the DLL at 0x00B60000,
 * not its preferred 0x10000000) -- replaced by the four bytes it points at:
 * the same code referring to the same constants passes wherever the loader or
 * the linker put them (Art of Conquest's copy keeps its constants elsewhere).
 * Import-table slots are left zero: their contents are only known at run time.
 * The replacements take their constants from the operands, never from fixed
 * addresses. */
static unsigned ssem_hash(const unsigned char *base, DWORD rva, unsigned len) {
  const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
  const IMAGE_DATA_DIRECTORY *rd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
  const IMAGE_DATA_DIRECTORY *iat = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT];
  const DWORD image = nt->OptionalHeader.SizeOfImage;
  unsigned char buf[1024];
  if (len > sizeof buf)
    return 0;
  memcpy(buf, base + rva, len);
  if (rd->VirtualAddress && rd->Size) {
    const unsigned char *p = base + rd->VirtualAddress, *end = p + rd->Size;
    while (p + sizeof(IMAGE_BASE_RELOCATION) <= end) {
      const IMAGE_BASE_RELOCATION *br = (const IMAGE_BASE_RELOCATION *)p;
      const WORD *e = (const WORD *)(p + sizeof *br);
      unsigned n, i;
      if (br->SizeOfBlock < sizeof *br)
        break;
      n = (br->SizeOfBlock - sizeof *br) / 2;
      if (br->VirtualAddress + 0x1000 > rva && br->VirtualAddress < rva + len)
        for (i = 0; i < n; i++)
          if ((e[i] >> 12) == IMAGE_REL_BASED_HIGHLOW) {
            const DWORD r = br->VirtualAddress + (e[i] & 0xfff);
            DWORD t, v = 0;
            unsigned k;
            if (r + 4 <= rva || r >= rva + len)
              continue;
            memcpy(&t, base + r, 4);
            t -= (DWORD)(ULONG_PTR)base; /* the target's RVA */
            if (t + 4 <= image && !(t >= iat->VirtualAddress && t < iat->VirtualAddress + iat->Size))
              memcpy(&v, base + t, 4);
            for (k = 0; k < 4; k++)
              if (r + k >= rva && r + k < rva + len)
                buf[r + k - rva] = ((const unsigned char *)&v)[k];
          }
      p += br->SizeOfBlock;
    }
  }
  return ssem_fnv(buf, len);
}

/* The absolute address in a function's instruction operand at +off. */
#define SSEM_OPND(fn, off) (*(const float *const *)((const unsigned char *)(fn) + (off)))

/* msvcrt's sqrt, if the call at fn+off goes through an import thunk to it. */
static double(__cdecl *ssem_sqrt_via(const unsigned char *fn, unsigned off))(double) {
  const unsigned char *c = fn + off, *t;
  void *crt = (void *)GetProcAddress(GetModuleHandleA("msvcrt.dll"), "sqrt"), *slot;
  int rel;
  if (c[0] != 0xE8)
    return NULL;
  memcpy(&rel, c + 1, 4);
  t = c + 5 + rel;
  if (IsBadReadPtr(t, 6) || t[0] != 0xFF || t[1] != 0x25)
    return NULL;
  memcpy(&slot, t + 2, 4);
  if (IsBadReadPtr(slot, 4) || !crt || *(void **)slot != crt)
    return NULL;
  return (double(__cdecl *)(double))crt;
}

/* Decides which replacements may go in (ok[i], by g_ssem_patches index): the
 * bytes and constants are the reversed ones, every call inside lands where the
 * replacement assumes, and every callee it copies is itself the reversed code.
 * Then takes the constants from the accepted functions' own operands.  Used by
 * ee_ssemath_install and by diagnostics/ssemath-test. */
static int ssem_find(const char *name);
static void ssem_prepare(HMODULE lle, int *ok, void (*logf)(const char *fmt, ...)) {
  const unsigned char *b = (const unsigned char *)lle;
  const unsigned char *mag2 = (const unsigned char *)GetProcAddress(lle, SSEM_MAG2);
  const int mag2_ok = mag2 && ssem_hash(b, (DWORD)(mag2 - b), SSEM_MAG2_LEN) == 0xd30e37d8u;
  int i, k, changed;
  for (i = 0; i < SSEM_N; i++) {
    const unsigned char *fn = (const unsigned char *)GetProcAddress(lle, g_ssem_patches[i].name);
    unsigned h;
    ok[i] = 0;
    if (!fn) {
      if (logf)
        logf("sse maths: %s is not exported -- skipped", g_ssem_patches[i].name);
      continue;
    }
    h = ssem_hash(b, (DWORD)(fn - b), g_ssem_patches[i].len);
    if (h != g_ssem_patches[i].fnv) {
      if (logf)
        logf("sse maths: %s is not the reversed code (fnv 0x%08x, want 0x%08x) -- left alone", g_ssem_patches[i].name, h,
             g_ssem_patches[i].fnv);
      continue;
    }
    ok[i] = 1;
  }
  do { /* a caller falls with its callee, so repeat until nothing changes */
    changed = 0;
    for (k = 0; k < (int)(sizeof g_ssem_calls / sizeof g_ssem_calls[0]); k++) {
      const int f = ssem_find(g_ssem_calls[k].fn);
      const unsigned char *fn, *c;
      int good, rel;
      if (f < 0 || !ok[f])
        continue;
      fn = (const unsigned char *)GetProcAddress(lle, g_ssem_calls[k].fn);
      c = fn + g_ssem_calls[k].off;
      if (!strcmp(g_ssem_calls[k].callee, "sqrt"))
        good = ssem_sqrt_via(fn, g_ssem_calls[k].off) != NULL;
      else {
        const int ci = ssem_find(g_ssem_calls[k].callee);
        memcpy(&rel, c + 1, 4);
        good = c[0] == 0xE8 && c + 5 + rel == (const unsigned char *)GetProcAddress(lle, g_ssem_calls[k].callee) &&
               (ci >= 0 ? ok[ci] : !strcmp(g_ssem_calls[k].callee, SSEM_MAG2) && mag2_ok);
      }
      if (!good) {
        ok[f] = 0;
        changed = 1;
        if (logf)
          logf("sse maths: %s calls %s at +0x%x, not as reversed -- left alone", g_ssem_calls[k].fn, g_ssem_calls[k].callee,
               g_ssem_calls[k].off);
      }
    }
  } while (changed);
  /* constants, from the accepted functions' own operands */
  if ((i = ssem_find(SSEM_COS)) >= 0 && ok[i]) {
    const unsigned char *fn = (const unsigned char *)GetProcAddress(lle, SSEM_COS);
    g_ssem.cos_lo = *SSEM_OPND(fn, 0x9);
    g_ssem.cos_step = *SSEM_OPND(fn, 0x16);
    g_ssem.cos_hi = *SSEM_OPND(fn, 0x20);
    g_ssem.cos_scale = *SSEM_OPND(fn, 0x48);
    g_ssem.cos_table = SSEM_OPND(fn, 0x5f);
  }
  if ((i = ssem_find(SSEM_YPR)) >= 0 && ok[i])
    g_ssem.half_pi = *SSEM_OPND(GetProcAddress(lle, SSEM_YPR), 0x1a);
  if ((i = ssem_find(SSEM_ACOS)) >= 0 && ok[i]) {
    const unsigned char *fn = (const unsigned char *)GetProcAddress(lle, SSEM_ACOS);
    g_ssem.ac_lim = *SSEM_OPND(fn, 0x18);
    g_ssem.ac_scale = *SSEM_OPND(fn, 0x23);
    g_ssem.ac_table = SSEM_OPND(fn, 0x36);
    g_ssem.ac_c3 = *SSEM_OPND(fn, 0x71);
    g_ssem.ac_c4 = *SSEM_OPND(fn, 0x82);
    g_ssem.ac_c5 = *SSEM_OPND(fn, 0x92);
    g_ssem.ac_clamp = *SSEM_OPND(fn, 0xb1);
    g_ssem.ac_pi = *SSEM_OPND(fn, 0xcf);
    g_ssem.half_pi = *SSEM_OPND(fn, 0xda);
    g_ssem.crt_sqrt = ssem_sqrt_via(fn, 0x58);
  }
  if ((i = ssem_find(SSEM_ATAN2)) >= 0 && ok[i]) {
    const unsigned char *fn = (const unsigned char *)GetProcAddress(lle, SSEM_ATAN2);
    g_ssem.ac_nhalf_pi = *SSEM_OPND(fn, 0x5f);
    g_ssem.half_pi = *SSEM_OPND(fn, 0x67);
  }
  if ((i = ssem_find(SSEM_LSPH)) >= 0 && ok[i])
    g_ssem.crt_sqrt = ssem_sqrt_via((const unsigned char *)GetProcAddress(lle, SSEM_LSPH), 0x107);
}

/* Jumps each accepted function to its replacement.  Call before any other
 * thread runs engine code (ee-version does it in DllMain).  Returns how many
 * were redirected; logf gets one line per function it leaves alone. */
static int ee_ssemath_install(HMODULE lle, void (*logf)(const char *fmt, ...)) {
  int ok[64], i, n = 0;
  ssem_prepare(lle, ok, logf);
  for (i = 0; i < SSEM_N; i++) {
    unsigned char *fn = (unsigned char *)GetProcAddress(lle, g_ssem_patches[i].name);
    DWORD old;
    int rel;
    if (!ok[i])
      continue;
    if (!VirtualProtect(fn, 5, PAGE_EXECUTE_READWRITE, &old)) {
      logf("sse maths: VirtualProtect(%s) failed %lu", g_ssem_patches[i].name, (unsigned long)GetLastError());
      continue;
    }
    rel = (int)((char *)g_ssem_patches[i].to - ((char *)fn + 5));
    fn[0] = 0xE9;
    memcpy(fn + 1, &rel, 4);
    VirtualProtect(fn, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, 5);
    n++;
  }
  return n;
}
static int ssem_find(const char *name) {
  int i;
  for (i = 0; i < SSEM_N; i++)
    if (!strcmp(g_ssem_patches[i].name, name))
      return i;
  return -1;
}

/* ---- DrawTerrainMaterial's vertex loop (DX7HRTnLDisplay.dll) --------------
 * The loop start (mov eax,[edx]; test; je exit) is replaced by a jump to
 * ssem_tstub, which fills every polygon's vertices in one go and resumes at the
 * loop's exit -- the vertex-buffer unlock and DrawPrimitive stay the game's.
 * Nothing after the loop reads the registers it used (edx, eax, ecx, edi) or
 * the output pointer at [ebp+0xc]. */
#define SSEM_TNL_DTM "?DrawTerrainMaterial@DX7Rasterizer@@MAEJPAVGEViewport@@AAV?$vector@PAUGETMPolygon@GETerrainMesh@@V?$allocator@PAUGETMPolygon@GETerrainMesh@@@std@@@std@@K@Z"
void ssem_tfill(const char *const *polys, char *out) {
  if (SSEM_PC24()) ssem_tfill_f(polys, out); else ssem_tfill_d(polys, out);
}
void *ssem_tresume;
void ssem_tstub(void);
__asm__(".text\n.globl _ssem_tstub\n_ssem_tstub:\n"
        "\tpushal\n"
        "\tpushl 0xc(%ebp)\n" /* the output pointer, in the game function's frame */
        "\tpushl %edx\n"      /* the polygon iterator */
        "\tcall _ssem_tfill\n"
        "\taddl $8, %esp\n"
        "\tpopal\n"
        "\tjmp *_ssem_tresume\n");

/* Returns 1 when the loop was redirected, 2 when it already was (the game
 * LoadLibrary's its renderers several times; a module that stays resident keeps
 * the patch, and one that was freed and reloaded comes back as the original
 * code and gets patched again), 0 when left alone.  dx7 = a DX7HR*Display.dll:
 * DX7HRDisplay exports the same method with different code and is left alone. */
static int ee_ssemath_terrain(HMODULE dx7, void (*logf)(const char *fmt, ...)) {
  static const unsigned char loop[10] = {0x8b, 0x02, 0x85, 0xc0, 0x0f, 0x84, 0x29, 0x02, 0x00, 0x00};
  static unsigned logged;
  unsigned char *fn = (unsigned char *)GetProcAddress(dx7, SSEM_TNL_DTM), *at;
  unsigned h;
  DWORD old;
  int rel;
  if (!fn)
    return 0;
  at = fn + 0x71;
  memcpy(&rel, at + 1, 4);
  if (at[0] == 0xE9 && at + 5 + rel == (unsigned char *)ssem_tstub)
    return 2;
  h = ssem_hash((const unsigned char *)dx7, (DWORD)(fn - (unsigned char *)dx7), 766);
  if (h != 0x26572550u || memcmp(at, loop, sizeof loop)) {
    if (h != logged)
      logf("sse maths: DrawTerrainMaterial is not the reversed code (fnv 0x%08x) -- left alone", h);
    logged = h;
    return 0;
  }
  g_ssem.terrain_k = *SSEM_OPND(fn, 0xb3); /* the colour scale, as the loop's first fmul reads it */
  ssem_tresume = fn + 0x2a4;
  if (!VirtualProtect(at, 5, PAGE_EXECUTE_READWRITE, &old))
    return 0;
  rel = (int)((char *)ssem_tstub - (char *)(at + 5));
  at[0] = 0xE9;
  memcpy(at + 1, &rel, 4);
  VirtualProtect(at, 5, old, &old);
  FlushInstructionCache(GetCurrentProcess(), at, 5);
  return 1;
}

/* ---- Animation Smoothing's keyframe blend (DX7HRTnLDisplay.dll) ------------
 * With 150+ animated units on screen the two x87 loops took half of every
 * frame (23 Sep 2026), so the launcher had turned Animation Smoothing off and
 * units stepped between keyframes.  Jumps each entry to its SSE version; like
 * the terrain loop it recognises its own jump when the renderer is loaded
 * again.  Returns how many of the two are redirected (already or now). */
#define SSEM_SMOOTH_STD "?CopyAndSmoothStdVertices@DX7Rasterizer@@IAEXPAVGEModel@@PAVGEMaterial@@PAUD3DVERTEX_STD_COLOR@@@Z"
#define SSEM_SMOOTH_COL "?CopyAndSmoothStdColorVertices@DX7Rasterizer@@IAEXPAVGEModel@@PAVGEMaterial@@PAUD3DVERTEX_STD_COLOR@@@Z"
static int ee_ssemath_smooth(HMODULE dx7, void (*logf)(const char *fmt, ...)) {
  static const struct { const char *name; unsigned len, fnv; void *to; } fns[2] = {
      {SSEM_SMOOTH_STD, 201, 0x83b22387u, (void *)ssem_smooth_std},
      {SSEM_SMOOTH_COL, 207, 0xc16eda13u, (void *)ssem_smooth_color},
  };
  static unsigned logged;
  int i, n = 0;
  for (i = 0; i < 2; i++) {
    unsigned char *fn = (unsigned char *)GetProcAddress(dx7, fns[i].name);
    unsigned h;
    DWORD old;
    int rel;
    if (!fn)
      continue;
    memcpy(&rel, fn + 1, 4);
    if (fn[0] == 0xE9 && fn + 5 + rel == (unsigned char *)fns[i].to) {
      n++;
      continue;
    }
    h = ssem_hash((const unsigned char *)dx7, (DWORD)(fn - (unsigned char *)dx7), fns[i].len);
    if (h != fns[i].fnv) {
      if (h != logged)
        logf("sse maths: %.40s is not the reversed code (fnv 0x%08x) -- left alone", fns[i].name, h);
      logged = h;
      continue;
    }
    if (!VirtualProtect(fn, 5, PAGE_EXECUTE_READWRITE, &old))
      continue;
    rel = (int)((unsigned char *)fns[i].to - (fn + 5));
    fn[0] = 0xE9;
    memcpy(fn + 1, &rel, 4);
    VirtualProtect(fn, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, 5);
    n++;
  }
  return n;
}
