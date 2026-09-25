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
  float cos_lo, cos_step, cos_hi, cos_scale, half_pi;
  const float *cos_table;
} g_ssem;

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
static float ssem_umcos(float a) { return SSEM_PC24() ? ssem_umcos_f(a) : ssem_umcos_d(a); }
static void SSEM_THIS ssem_bbox_dims(float *b) {
  if (SSEM_PC24()) ssem_bbox_dims_f(b); else ssem_bbox_dims_d(b);
}
static void SSEM_THIS ssem_ypr(float *m, float a, float b, float c) {
  if (SSEM_PC24()) ssem_ypr_f(m, a, b, c); else ssem_ypr_d(m, a, b, c);
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
    SSEM_P("?UMCos@@YAMM@Z", 101, 0xeb6ec416, umcos),
    {"??0GETransformation@@QAE@XZ", 82, 0x1a835b38, (void *)ssem_ctor},
    {"??0GE3DPlane@@QAE@XZ", 37, 0xe15509de, (void *)ssem_plane_ctor},
    {"??0GE3DLine@@QAE@XZ", 32, 0x445ed7ea, (void *)ssem_line_ctor},
    {"??0GEBoundingBox@@QAE@XZ", 41, 0x895a38a8, (void *)ssem_bbox_ctor},
    SSEM_P("?ComputeDimensions@GEBoundingBox@@AAEXXZ", 105, 0x46cedc3a, bbox_dims),
    SSEM_P("?SetOrientationYPR@GETransformation@@QAEXMMM@Z", 244, 0x57ef8956, ypr),
};
#define SSEM_N ((int)(sizeof g_ssem_patches / sizeof g_ssem_patches[0]))

static unsigned ssem_fnv(const unsigned char *p, unsigned n) {
  unsigned h = 2166136261u;
  while (n--)
    h = (h ^ *p++) * 16777619u;
  return h;
}

/* Reads UMCos's constants and table from the loaded DLL (lle = its base). */
static void ssem_bind(HMODULE lle) {
  const char *b = (const char *)lle;
  g_ssem.cos_lo = *(const float *)(b + 0x96534);
  g_ssem.cos_step = *(const float *)(b + 0x96538);
  g_ssem.cos_hi = *(const float *)(b + 0x9653c);
  g_ssem.cos_scale = *(const float *)(b + 0x96744);
  g_ssem.cos_table = (const float *)(b + 0xd9130);
  g_ssem.half_pi = *(const float *)(b + 0x94818);
}

/* FNV-1a of a function's bytes with every base-relocated dword zeroed, so the
 * check holds wherever the loader put the DLL (the game loads it at 0x00B60000,
 * not its preferred 0x10000000, which rewrites the absolute addresses). */
static unsigned ssem_hash(const unsigned char *base, DWORD rva, unsigned len) {
  const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
  const IMAGE_DATA_DIRECTORY *rd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
  unsigned char buf[512];
  if (len > sizeof buf)
    return 0;
  memcpy(buf, base + rva, len);
  if (rd->VirtualAddress && rd->Size) {
    const unsigned char *p = base + rd->VirtualAddress, *end = p + rd->Size;
    while (p + sizeof(IMAGE_BASE_RELOCATION) <= end) {
      const IMAGE_BASE_RELOCATION *br = (const IMAGE_BASE_RELOCATION *)p;
      const WORD *e = (const WORD *)(p + sizeof *br);
      unsigned n, i, k;
      if (br->SizeOfBlock < sizeof *br)
        break;
      n = (br->SizeOfBlock - sizeof *br) / 2;
      if (br->VirtualAddress + 0x1000 > rva && br->VirtualAddress < rva + len)
        for (i = 0; i < n; i++)
          if ((e[i] >> 12) == IMAGE_REL_BASED_HIGHLOW)
            for (k = 0; k < 4; k++) {
              DWORD r = br->VirtualAddress + (e[i] & 0xfff) + k;
              if (r >= rva && r < rva + len)
                buf[r - rva] = 0;
            }
      p += br->SizeOfBlock;
    }
  }
  return ssem_fnv(buf, len);
}

/* Jumps each reversed function to its replacement.  Call before any other
 * thread runs engine code (ee-version does it in DllMain).  Returns how many
 * were redirected; logf gets one line per function it leaves alone. */
static int ee_ssemath_install(HMODULE lle, void (*logf)(const char *fmt, ...)) {
  int i, n = 0;
  ssem_bind(lle);
  for (i = 0; i < SSEM_N; i++) {
    const struct ssem_patch *p = &g_ssem_patches[i];
    unsigned char *fn = (unsigned char *)GetProcAddress(lle, p->name);
    unsigned h;
    DWORD old;
    int rel;
    if (!fn) {
      logf("sse maths: %s is not exported -- skipped", p->name);
      continue;
    }
    h = ssem_hash((const unsigned char *)lle, (DWORD)(fn - (unsigned char *)lle), p->len);
    if (h != p->fnv) {
      logf("sse maths: %s is not the reversed code (fnv 0x%08x, want 0x%08x) -- left alone", p->name, h, p->fnv);
      continue;
    }
    if (!VirtualProtect(fn, 5, PAGE_EXECUTE_READWRITE, &old)) {
      logf("sse maths: VirtualProtect(%s) failed %lu", p->name, (unsigned long)GetLastError());
      continue;
    }
    rel = (int)((char *)p->to - ((char *)fn + 5));
    fn[0] = 0xE9;
    memcpy(fn + 1, &rel, 4);
    VirtualProtect(fn, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, 5);
    n++;
  }
  return n;
}
