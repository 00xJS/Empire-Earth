/* x87 -> SSE2 translation of whole game functions (EE_X87T).
 *
 * The simulation thread runs the x87 at 53-bit precision (control word 0x027f),
 * where every x87 result rounds exactly as the SSE2 double operation does.  So
 * a function's x87 code can run on SSE2 unchanged in effect if the x87 register
 * stack is kept in xmm registers: stack slot p (0 = bottom) lives in xmm<p>,
 * xmm7 is scratch.  Floats load with cvtss2sd and store with cvtsd2ss, the same
 * double rounding as fld/fstp dword at 53 bits; compares set EFLAGS with ucomisd
 * and fnstsw becomes lahf, which puts ZF/PF/CF where C3/C2/C0 sit in AH.
 *
 * diagnostics/x87t-gen.py does the analysis offline (x87 depth over the control
 * flow, which calls return a float in st(0), flags and AL liveness) and writes a
 * recipe per function into ee-x87t-recipes.h: offsets into the game's code and
 * the operations below -- none of the game's bytes.  Here each recipe is
 * replayed against the running game, after the function's bytes are checked,
 * into a new function; the original's entry jumps to a dispatcher that sends
 * callers at 53-bit precision and round-to-nearest there and everyone else
 * (the render thread runs at 24 bits) through a trampoline to the original. */

struct x87t_fn { /* Empire Earth.exe, at a fixed address */
  DWORD addr;
  unsigned size, fnv, prologue, pcall; /* pcall: offset of a call in the prologue, 0xff: none */
  const unsigned char *recipe;
  const char *name;
  unsigned single; /* 1: for 24-bit callers (the render thread), 0: for 53-bit ones */
};
struct x87t_dllfn { /* Low-Level Engine.dll, by export */
  const char *export;
  unsigned size, fnv, prologue, pcall;
  const unsigned char *recipe;
  unsigned single;
};
#include "ee-x87t-recipes.h"            /* Empire Earth.exe */
#include "ee-x87t-aoc-recipes.h"        /* Art of Conquest's EE-AOC.exe */
#include "ee-x87t-dll-recipes.h"        /* Low-Level Engine.dll, either game: the simulation's */
#include "ee-x87t-dll-single-recipes.h" /* Low-Level Engine.dll: the render thread's */
#include "ee-x87t-dll-trial-recipes.h"  /* ...still being checked in play: EE_X87T_SHADOW=1 only */

enum {
  X87T_END = 0x00, X87T_COPY = 0x01, X87T_LABEL = 0x02, X87T_JCC = 0x03, X87T_JMP = 0x04, X87T_CALL = 0x05,
  X87T_JMPOUT = 0x06, X87T_LOAD = 0x10, X87T_STORE = 0x11, X87T_ARITHM = 0x12, X87T_ARITHR = 0x13,
  X87T_MOV = 0x14, X87T_CONST = 0x15, X87T_UNARY = 0x16, X87T_CMPR = 0x17, X87T_CMPM = 0x18, X87T_CMPZ = 0x19,
  X87T_FNSTSW = 0x1a, X87T_CALLF = 0x1b, X87T_RETF = 0x1c, X87T_SWAP = 0x1d, X87T_FILD64 = 0x1e
};

static const unsigned long long x87t_consts[6] __attribute__((aligned(16))) = {
    0x3ff0000000000000ULL, 0,                     /* +0: 1.0 */
    0x8000000000000000ULL, 0x8000000000000000ULL, /* +16: sign mask (xorpd) */
    0x7fffffffffffffffULL, 0x7fffffffffffffffULL, /* +32: abs mask (andpd) */
};
static const unsigned x87t_fconsts[12] __attribute__((aligned(16))) = {
    0x3f800000u, 0, 0, 0,                                 /* +0: 1.0f */
    0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u, /* +16: sign mask (xorps) */
    0x7fffffffu, 0x7fffffffu, 0x7fffffffu, 0x7fffffffu, /* +32: abs mask (andps) */
};

/* The two precisions: doubles for the simulation thread's 53-bit x87, floats
 * for the render thread's 24-bit one (x87t-gen.py --single: float operands only). */
struct x87t_mode {
  int single;
  unsigned char arith; /* F2: addsd.., F3: addss.. */
  unsigned char vec;   /* 66: movapd/xorpd/andpd/ucomisd, 0: the ps forms */
  const void *one, *sign, *absm;
};
static const struct x87t_mode x87t_modes[2] = {
    {0, 0xF2, 0x66, &x87t_consts[0], &x87t_consts[2], &x87t_consts[4]},
    {1, 0xF3, 0x00, &x87t_fconsts[0], &x87t_fconsts[4], &x87t_fconsts[8]},
};

/* Length of the ModRM operand (ModRM, SIB, displacement) at m. */
static unsigned x87t_modrm_len(const unsigned char *m) {
  const unsigned mod = m[0] >> 6, rm = m[0] & 7;
  unsigned n = 1;
  if (mod != 3 && rm == 4) {
    n++;
    if (mod == 0 && (m[1] & 7) == 5)
      n += 4;
  }
  if (mod == 1)
    n += 1;
  else if (mod == 2 || (mod == 0 && rm == 5))
    n += 4;
  return n;
}

struct x87t_fix {
  unsigned char *at;
  unsigned short t;
};
struct x87t_out {
  unsigned char *p, *end;
  int bad;
};
static void x87t_b(struct x87t_out *o, const unsigned char *b, unsigned n) {
  if (o->p + n > o->end) {
    o->bad = 1;
    return;
  }
  memcpy(o->p, b, n);
  o->p += n;
}
#define X87T_EMIT(o, ...)                                                                                  \
  do {                                                                                                     \
    static const unsigned char b_[] = {__VA_ARGS__};                                                       \
    x87t_b(o, b_, sizeof b_);                                                                              \
  } while (0)
static void x87t_rel32(struct x87t_out *o, const unsigned char *to) {
  int rel = (int)(to - (o->p + 4));
  x87t_b(o, (const unsigned char *)&rel, 4);
}
/* prefix, 0F, op, then a ModRM with reg = r and the memory operand of the x87
 * instruction at insn (its ModRM's reg field was the x87 operation) */
static void x87t_mem(struct x87t_out *o, unsigned char pfx, unsigned char op, unsigned r, const unsigned char *insn) {
  const unsigned n = x87t_modrm_len(insn + 1);
  unsigned char h[4];
  unsigned k = 0;
  if (pfx)
    h[k++] = pfx;
  h[k++] = 0x0F;
  h[k++] = op;
  h[k++] = (unsigned char)((insn[1] & 0xC7) | (r << 3));
  x87t_b(o, h, k);
  x87t_b(o, insn + 2, n - 1);
}
static void x87t_rr(struct x87t_out *o, unsigned char pfx, unsigned char op, unsigned r, unsigned m) {
  const unsigned char h[4] = {pfx, 0x0F, op, (unsigned char)(0xC0 | (r << 3) | m)};
  x87t_b(o, pfx ? h : h + 1, pfx ? 4 : 3);
}
static void x87t_abs(struct x87t_out *o, unsigned char pfx, unsigned char op, unsigned r, const void *addr) {
  const unsigned char h[4] = {pfx, 0x0F, op, (unsigned char)(0x05 | (r << 3))};
  const DWORD a = (DWORD)(ULONG_PTR)addr;
  x87t_b(o, pfx ? h : h + 1, pfx ? 4 : 3);
  x87t_b(o, (const unsigned char *)&a, 4);
}
/* xmm7 = the operand of the x87 instruction at insn, by kind: 0 float, 1 double,
 * 2 int32 (the last two for doubles only: --single refuses them) */
static void x87t_load7(struct x87t_out *o, const struct x87t_mode *m, unsigned kind, const unsigned char *insn) {
  if (kind == 0)
    x87t_mem(o, 0xF3, m->single ? 0x10 : 0x5A, 7, insn); /* movss / cvtss2sd */
  else if (kind == 1 && !m->single)
    x87t_mem(o, 0xF2, 0x10, 7, insn); /* movsd */
  else if (kind == 2 && !m->single)
    x87t_mem(o, 0xF2, 0x2A, 7, insn); /* cvtsi2sd */
  else
    o->bad = 1;
}
static const unsigned char x87t_sse_op[8] = {0x58, 0x59, 0, 0, 0x5C, 0x5C, 0x5E, 0x5E}; /* add mul - - sub subr div divr */

/* Replay one recipe.  src: the function's bytes; va: where they run (for the
 * targets of its calls).  Returns the end of the code, or NULL. */
static unsigned char *x87t_build(const struct x87t_fn *f, const unsigned char *src, DWORD va, unsigned char *code,
                                 unsigned char *limit, unsigned char **label, struct x87t_fix *fix, unsigned nfixmax,
                                 int single) {
  const struct x87t_mode *m = &x87t_modes[single ? 1 : 0];
  struct x87t_out o = {code, limit, 0};
  const unsigned char *r = f->recipe;
  unsigned nfix = 0, k;
#define U16(p) ((unsigned)((p)[0] | ((p)[1] << 8)))
  memset(label, 0, sizeof(*label) * (f->size + 1));
  label[0] = code;
  while (*r != X87T_END && !o.bad) {
    const unsigned char op = *r++;
    switch (op) {
    case X87T_COPY:
      x87t_b(&o, src + U16(r), r[2]);
      r += 3;
      break;
    case X87T_LABEL:
      label[U16(r)] = o.p;
      r += 2;
      break;
    case X87T_JCC: {
      const unsigned char h[2] = {0x0F, (unsigned char)(0x80 | r[0])};
      x87t_b(&o, h, 2);
      if (nfix < nfixmax) {
        fix[nfix].at = o.p;
        fix[nfix++].t = (unsigned short)U16(r + 1);
      } else
        o.bad = 1;
      x87t_b(&o, (const unsigned char *)"\0\0\0\0", 4);
      r += 3;
      break;
    }
    case X87T_JMP:
      X87T_EMIT(&o, 0xE9);
      if (nfix < nfixmax) {
        fix[nfix].at = o.p;
        fix[nfix++].t = (unsigned short)U16(r);
      } else
        o.bad = 1;
      x87t_b(&o, (const unsigned char *)"\0\0\0\0", 4);
      r += 2;
      break;
    case X87T_CALL:
    case X87T_JMPOUT: {
      const unsigned off = U16(r);
      const unsigned char *in = src + off;
      int rel;
      DWORD to;
      if (in[0] == 0xEB)
        to = va + off + 2 + (DWORD)(int)(signed char)in[1];
      else {
        memcpy(&rel, in + 1, 4);
        to = va + off + 5 + (DWORD)rel;
      }
      {
        const unsigned char c = op == X87T_CALL ? 0xE8 : 0xE9;
        x87t_b(&o, &c, 1);
      }
      x87t_rel32(&o, (const unsigned char *)(ULONG_PTR)to);
      r += 2;
      break;
    }
    case X87T_LOAD: { /* kind, reg, off */
      const unsigned char *in = src + U16(r + 2);
      if (r[0] == 0)
        x87t_mem(&o, 0xF3, m->single ? 0x10 : 0x5A, r[1], in); /* movss / cvtss2sd */
      else if (r[0] == 1 && !m->single)
        x87t_mem(&o, 0xF2, 0x10, r[1], in); /* movsd */
      else if (r[0] == 2 && !m->single)
        x87t_mem(&o, 0xF2, 0x2A, r[1], in); /* cvtsi2sd */
      else
        o.bad = 1;
      r += 4;
      break;
    }
    case X87T_STORE: { /* kind, reg, off */
      const unsigned char *in = src + U16(r + 2);
      if (r[0] == 0) {
        if (m->single)
          x87t_mem(&o, 0xF3, 0x11, r[1], in); /* movss m32, reg */
        else {
          x87t_rr(&o, 0xF2, 0x5A, 7, r[1]); /* cvtsd2ss xmm7, reg */
          x87t_mem(&o, 0xF3, 0x11, 7, in); /* movss m32, xmm7 */
        }
      } else if (r[0] == 1) {
        if (m->single) {
          x87t_rr(&o, 0xF3, 0x5A, 7, r[1]); /* cvtss2sd xmm7, reg: exact */
          x87t_mem(&o, 0xF2, 0x11, 7, in); /* movsd m64, xmm7 */
        } else
          x87t_mem(&o, 0xF2, 0x11, r[1], in); /* movsd m64, reg */
      } else {
        /* cvtpd2dq / cvtps2dq xmm7, reg: round to nearest, as fistp; then movd m32, xmm7 */
        x87t_rr(&o, m->single ? 0x66 : 0xF2, m->single ? 0x5B : 0xE6, 7, r[1]);
        x87t_mem(&o, 0x66, 0x7E, 7, in);
      }
      r += 4;
      break;
    }
    case X87T_ARITHM: { /* op, kind, reg, off */
      const unsigned a = r[0], reg = r[2];
      x87t_load7(&o, m, r[1], src + U16(r + 3));
      if (a == 5 || a == 7) { /* reversed: reg = xmm7 op reg */
        x87t_rr(&o, m->arith, x87t_sse_op[a], 7, reg);
        x87t_rr(&o, m->vec, 0x28, reg, 7);
      } else
        x87t_rr(&o, m->arith, x87t_sse_op[a], reg, 7);
      r += 5;
      break;
    }
    case X87T_ARITHR: { /* op, dst, src: dst = dst op src */
      const unsigned a = r[0], d = r[1], s = r[2];
      if (a == 5 || a == 7) { /* dst = src op dst */
        x87t_rr(&o, m->vec, 0x28, 7, s);
        x87t_rr(&o, m->arith, x87t_sse_op[a], 7, d);
        x87t_rr(&o, m->vec, 0x28, d, 7);
      } else
        x87t_rr(&o, m->arith, x87t_sse_op[a], d, s);
      r += 3;
      break;
    }
    case X87T_MOV:
      if (r[0] != r[1])
        x87t_rr(&o, m->vec, 0x28, r[0], r[1]); /* movapd / movaps */
      r += 2;
      break;
    case X87T_CONST:
      if (r[0] == 0)
        x87t_rr(&o, m->vec, 0x57, r[1], r[1]); /* xorpd / xorps: +0.0 */
      else
        x87t_abs(&o, m->arith, 0x10, r[1], m->one); /* movsd / movss reg, [1.0] */
      r += 2;
      break;
    case X87T_UNARY:
      if (r[0] == 0)
        x87t_abs(&o, m->vec, 0x57, r[1], m->sign); /* xorp. reg, [sign] */
      else if (r[0] == 1)
        x87t_abs(&o, m->vec, 0x54, r[1], m->absm); /* andp. reg, [abs] */
      else
        x87t_rr(&o, m->arith, 0x51, r[1], r[1]); /* sqrts. */
      r += 2;
      break;
    case X87T_CMPR:
      x87t_rr(&o, m->vec, 0x2E, r[0], r[1]); /* ucomis. a, b */
      r += 2;
      break;
    case X87T_CMPM:
      x87t_load7(&o, m, r[0], src + U16(r + 2));
      x87t_rr(&o, m->vec, 0x2E, r[1], 7);
      r += 4;
      break;
    case X87T_CMPZ:
      x87t_rr(&o, m->vec, 0x57, 7, 7);
      x87t_rr(&o, m->vec, 0x2E, r[0], 7);
      r += 1;
      break;
    case X87T_FNSTSW:
      X87T_EMIT(&o, 0x9F); /* lahf */
      break;
    case X87T_CALLF: /* st(0) -> xmm<reg>, through the stack: fstp qword / dword, then movsd / movss */
      if (m->single)
        X87T_EMIT(&o, 0x8D, 0x64, 0x24, 0xFC, 0xD9, 0x1C, 0x24);
      else
        X87T_EMIT(&o, 0x8D, 0x64, 0x24, 0xF8, 0xDD, 0x1C, 0x24);
      {
        const unsigned char h[5] = {m->single ? 0xF3 : 0xF2, 0x0F, 0x10, (unsigned char)(0x04 | (r[0] << 3)), 0x24};
        x87t_b(&o, h, 5);
      }
      if (m->single)
        X87T_EMIT(&o, 0x8D, 0x64, 0x24, 0x04);
      else
        X87T_EMIT(&o, 0x8D, 0x64, 0x24, 0x08);
      r += 1;
      break;
    case X87T_RETF: /* xmm<reg> -> st(0) */
      if (m->single)
        X87T_EMIT(&o, 0x8D, 0x64, 0x24, 0xFC);
      else
        X87T_EMIT(&o, 0x8D, 0x64, 0x24, 0xF8);
      {
        const unsigned char h[5] = {m->single ? 0xF3 : 0xF2, 0x0F, 0x11, (unsigned char)(0x04 | (r[0] << 3)), 0x24};
        x87t_b(&o, h, 5);
      }
      if (m->single)
        X87T_EMIT(&o, 0xD9, 0x04, 0x24, 0x8D, 0x64, 0x24, 0x04);
      else
        X87T_EMIT(&o, 0xDD, 0x04, 0x24, 0x8D, 0x64, 0x24, 0x08);
      r += 1;
      break;
    case X87T_FILD64: { /* reg, off: the game's own fild qword, then st(0) -> xmm through the stack
                         * (as fild then fstp qword: exact while the integer is below 2^53) */
      const unsigned char *in = src + U16(r + 1);
      if (m->single) {
        o.bad = 1;
        break;
      }
      x87t_b(&o, in, 1 + x87t_modrm_len(in + 1));
      X87T_EMIT(&o, 0x8D, 0x64, 0x24, 0xF8, 0xDD, 0x1C, 0x24);
      {
        const unsigned char h[5] = {0xF2, 0x0F, 0x10, (unsigned char)(0x04 | (r[0] << 3)), 0x24};
        x87t_b(&o, h, 5);
      }
      X87T_EMIT(&o, 0x8D, 0x64, 0x24, 0x08);
      r += 3;
      break;
    }
    case X87T_SWAP:
      x87t_rr(&o, m->vec, 0x28, 7, r[0]);
      x87t_rr(&o, m->vec, 0x28, r[0], r[1]);
      x87t_rr(&o, m->vec, 0x28, r[1], 7);
      r += 2;
      break;
    default:
      return NULL;
    }
  }
  if (o.bad)
    return NULL;
  for (k = 0; k < nfix; k++) {
    const unsigned char *to = fix[k].t <= f->size ? label[fix[k].t] : NULL;
    int rel;
    if (!to)
      return NULL;
    rel = (int)(to - (fix[k].at + 4));
    memcpy(fix[k].at, &rel, 4);
  }
#undef U16
  return o.p;
}

/* The dispatcher in front of a translated body, then the trampoline to the
 * original: callers at the body's precision and round-to-nearest (high byte of
 * the control word 0x02 for 53 bits, 0x00 for 24) take the body, everyone else
 * the game's own code. */
static unsigned char *x87t_dispatcher(unsigned char *p, const unsigned char *body, const unsigned char *tramp,
                                      int single) {
  static const unsigned char h[] = {0x8D, 0x64, 0x24, 0xFC,       /* lea esp,[esp-4] */
                                    0xD9, 0x3C, 0x24,             /* fnstcw [esp] */
                                    0x80, 0x7C, 0x24, 0x01, 0x02, /* cmp byte [esp+1],2 (0 for --single) */
                                    0x8D, 0x64, 0x24, 0x04,       /* lea esp,[esp+4] */
                                    0x0F, 0x85};                  /* jne tramp */
  int rel;
  memcpy(p, h, sizeof h);
  if (single)
    p[11] = 0x00; /* high byte of the control word: 24-bit precision, round to nearest */
  p += sizeof h;
  rel = (int)(tramp - (p + 4));
  memcpy(p, &rel, 4);
  p += 4;
  *p++ = 0xE9; /* jmp body */
  rel = (int)(body - (p + 4));
  memcpy(p, &rel, 4);
  return p + 4;
}

static unsigned x87t_fnv(const unsigned char *p, unsigned n) {
  unsigned h = 2166136261u;
  while (n--)
    h = (h ^ *p++) * 16777619u;
  return h;
}

/* The relocated engine's bytes, hashed as x87t-gen.py does: each relocated
 * dword by what it points at (ssem_subst), and the displacement of every call
 * or jump out that the recipe re-targets zeroed -- they land wherever the
 * running module says, the translation calls the same place. */
static unsigned x87t_dllhash(const unsigned char *base, const unsigned char *fn, unsigned size, const unsigned char *r) {
  static unsigned char buf[8192];
  if (size > sizeof buf)
    return 0;
  memcpy(buf, fn, size);
  ssem_subst(buf, base, (DWORD)(fn - base), size);
  while (*r != X87T_END) {
    static const unsigned char len[0x1f] = {0, 3, 2, 3, 2, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                            4, 4, 5, 3, 2, 2, 2, 2, 4, 1, 0, 1, 1, 2, 3};
    const unsigned op = *r++;
    if (op >= sizeof len)
      return 0;
    if (op == X87T_CALL || op == X87T_JMPOUT) {
      const unsigned off = r[0] | (r[1] << 8);
      const unsigned n = fn[off] == 0xEB ? 1 : 4;
      if (off + 1 + n <= size)
        memset(buf + off + 1, 0, n);
    }
    r += len[op];
  }
  return x87t_fnv(buf, size);
}

/* Build one translation after its bytes checked out, put the dispatcher in
 * front and the trampoline behind, and point the original's entry at it. */
static int x87t_one(unsigned char *fn, unsigned size, unsigned prologue, unsigned pcall, const unsigned char *recipe,
                    int single, unsigned char **pp, unsigned char *end, unsigned *bytes) {
  struct x87t_fn f;
  unsigned char **label, *disp, *body, *tramp, *bend, *p = *pp;
  struct x87t_fix *fix;
  DWORD old;
  int rel;
  f.addr = (DWORD)(ULONG_PTR)fn;
  f.size = size;
  f.recipe = recipe;
  label = (unsigned char **)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*label) * (size + 1));
  fix = (struct x87t_fix *)HeapAlloc(GetProcessHeap(), 0, sizeof(*fix) * 1024);
  if (!label || !fix)
    return 0;
  p = (unsigned char *)(((ULONG_PTR)p + 15) & ~(ULONG_PTR)15);
  disp = p;
  body = disp + 32;
  bend = x87t_build(&f, fn, f.addr, body, end - 64, label, fix, 1024, single);
  HeapFree(GetProcessHeap(), 0, label);
  HeapFree(GetProcessHeap(), 0, fix);
  if (!bend)
    return 0;
  tramp = bend;
  memcpy(tramp, fn, prologue);
  if (pcall != 0xff) { /* a call among the copied bytes: to the same target from here */
    const unsigned char *to;
    memcpy(&rel, fn + pcall + 1, 4);
    to = fn + pcall + 5 + rel;
    rel = (int)(to - (tramp + pcall + 5));
    memcpy(tramp + pcall + 1, &rel, 4);
  }
  tramp[prologue] = 0xE9;
  rel = (int)((fn + prologue) - (tramp + prologue + 5));
  memcpy(tramp + prologue + 1, &rel, 4);
  x87t_dispatcher(disp, body, tramp, single);
  *pp = tramp + prologue + 5;
  *bytes += (unsigned)(*pp - disp);
  if (!VirtualProtect(fn, 5, PAGE_EXECUTE_READWRITE, &old))
    return 0;
  fn[0] = 0xE9;
  rel = (int)(disp - (fn + 5));
  memcpy(fn + 1, &rel, 4);
  VirtualProtect(fn, 5, old, &old);
  return 1;
}

/* On unless EE_X87T=0; EE_X87T=<list> translates only the functions whose hex
 * address (the exe's) or export name (the engine's) is in it -- to bisect.
 * exe_fns is the table for the host (Empire Earth.exe's or Art of Conquest's);
 * the engine's is checked by hash, so either game's copy takes what matches.  From DllMain, before any game
 * thread uses the code (in AoC: the plugin scan, see g_ee_exe). */
static const struct x87t_dllfn *const x87t_dll_tables[] = {x87t_dll_fns, x87t_dll_single_fns};

/* ---- shadow check (EE_X87T_SHADOW=1, diagnostics) ----------------------------
 * The render thread's picking and culling tests (GEPhysicalModel::Intersects,
 * x87t_dll_trial_fns) translate in single precision, but a test needs real
 * models.  In shadow mode each call from a 24-bit caller runs the game's code
 * and then the translation on the same arguments, compares the answer and the
 * hit distance (an in/out float: restored before the second run), counts, and
 * returns the game's own result.  Other callers get only the game's code. */
static struct x87t_shadow {
  const char *export;
  void *tramp, *body;
  volatile LONG n, bad;
} x87t_sh[3] = {
    {"?Intersects@GEPhysicalModel@@QBE_NABVGEViewport@@_N@Z", NULL, NULL, 0, 0},
    {"?Intersects@GEPhysicalModel@@ABE_NABVGETransformation@@PBVGEModel@@KABVGE3DLine@@KAAM_N@Z", NULL, NULL, 0, 0},
    {"?Intersects@GEPhysicalModel@@ABE_NABVGETransformation@@PBVGEModel@@KABVGE3DLine@@_NKMMM@Z", NULL, NULL, 0, 0},
};
static void (*x87t_sh_log)(const char *fmt, ...);
static int x87t_pc24(void) {
  unsigned short cw;
  __asm__ volatile("fnstcw %0" : "=m"(cw));
  return (cw & 0x0f00) == 0;
}
static void x87t_sh_count(struct x87t_shadow *s, int bad) {
  const LONG n = InterlockedIncrement(&s->n);
  if (bad && InterlockedIncrement(&s->bad) <= 5)
    x87t_sh_log("x87 shadow: %.60s differs (call %ld)", s->export, n);
  if (n % 500000 == 0)
    x87t_sh_log("x87 shadow: %.60s: %ld calls checked, %ld differ", s->export, n, s->bad);
}
typedef unsigned(__attribute__((thiscall)) * x87t_vp_fn)(void *, void *, int);
static unsigned __attribute__((thiscall)) x87t_sh_viewport(void *self, void *vp, int flag) {
  struct x87t_shadow *s = &x87t_sh[0];
  const unsigned r1 = ((x87t_vp_fn)s->tramp)(self, vp, flag);
  if (x87t_pc24())
    x87t_sh_count(s, (r1 & 0xff) != (((x87t_vp_fn)s->body)(self, vp, flag) & 0xff));
  return r1;
}
typedef unsigned(__attribute__((thiscall)) * x87t_pk_fn)(void *, void *, void *, unsigned, void *, unsigned, float *, int);
static unsigned __attribute__((thiscall)) x87t_sh_pick(void *self, void *xf, void *m, unsigned a, void *line, unsigned b,
                                                       float *out, int flag) {
  struct x87t_shadow *s = &x87t_sh[1];
  const float in = *out;
  const unsigned r1 = ((x87t_pk_fn)s->tramp)(self, xf, m, a, line, b, out, flag);
  if (x87t_pc24()) {
    const float o1 = *out;
    unsigned r2;
    *out = in;
    r2 = ((x87t_pk_fn)s->body)(self, xf, m, a, line, b, out, flag);
    x87t_sh_count(s, (r1 & 0xff) != (r2 & 0xff) || memcmp(&o1, out, 4));
    *out = o1;
  }
  return r1;
}
typedef unsigned(__attribute__((thiscall)) * x87t_pv_fn)(void *, void *, void *, unsigned, void *, int, unsigned, float,
                                                          float, float);
static unsigned __attribute__((thiscall)) x87t_sh_pickv(void *self, void *xf, void *m, unsigned a, void *line, int flag,
                                                        unsigned b, float x, float y, float z) {
  struct x87t_shadow *s = &x87t_sh[2];
  const unsigned r1 = ((x87t_pv_fn)s->tramp)(self, xf, m, a, line, flag, b, x, y, z);
  if (x87t_pc24())
    x87t_sh_count(s, (r1 & 0xff) != (((x87t_pv_fn)s->body)(self, xf, m, a, line, flag, b, x, y, z) & 0xff));
  return r1;
}
static void *const x87t_sh_wrap[3] = {(void *)x87t_sh_viewport, (void *)x87t_sh_pick, (void *)x87t_sh_pickv};

/* Build each trial function's body and trampoline and point its entry at the
 * shadow wrapper instead of a dispatcher. */
static unsigned x87t_shadow_install(HMODULE lle, unsigned char **pp, unsigned char *end, void (*logf)(const char *fmt, ...)) {
  unsigned i, k, done = 0;
  x87t_sh_log = logf;
  for (i = 0; x87t_dll_trial_fns[i].export; i++) {
    const struct x87t_dllfn *f = &x87t_dll_trial_fns[i];
    unsigned char *fn = (unsigned char *)GetProcAddress(lle, f->export), *body, *bend, *tramp, **label;
    struct x87t_fix *fix;
    struct x87t_fn tf;
    DWORD old;
    int rel;
    for (k = 0; k < 3 && strcmp(x87t_sh[k].export, f->export); k++)
      ;
    if (k == 3 || !fn || x87t_dllhash((const unsigned char *)lle, fn, f->size, f->recipe) != f->fnv)
      continue;
    label = (unsigned char **)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*label) * (f->size + 1));
    fix = (struct x87t_fix *)HeapAlloc(GetProcessHeap(), 0, sizeof(*fix) * 1024);
    if (!label || !fix)
      break;
    tf.addr = (DWORD)(ULONG_PTR)fn;
    tf.size = f->size;
    tf.recipe = f->recipe;
    body = (unsigned char *)(((ULONG_PTR)*pp + 15) & ~(ULONG_PTR)15);
    bend = x87t_build(&tf, fn, tf.addr, body, end - 64, label, fix, 1024, 1);
    HeapFree(GetProcessHeap(), 0, label);
    HeapFree(GetProcessHeap(), 0, fix);
    if (!bend)
      continue;
    tramp = bend;
    memcpy(tramp, fn, f->prologue);
    tramp[f->prologue] = 0xE9;
    rel = (int)((fn + f->prologue) - (tramp + f->prologue + 5));
    memcpy(tramp + f->prologue + 1, &rel, 4);
    *pp = tramp + f->prologue + 5;
    x87t_sh[k].tramp = tramp;
    x87t_sh[k].body = body;
    if (!VirtualProtect(fn, 5, PAGE_EXECUTE_READWRITE, &old))
      continue;
    fn[0] = 0xE9;
    rel = (int)((unsigned char *)x87t_sh_wrap[k] - (fn + 5));
    memcpy(fn + 1, &rel, 4);
    VirtualProtect(fn, 5, old, &old);
    done++;
  }
  return done;
}

static void x87t_install(const struct x87t_fn *exe_fns, HMODULE lle, void (*logf)(const char *fmt, ...)) {
  char spec[1024];
  unsigned i, t, done = 0, total = 0, bytes = 0;
  unsigned char *code, *p, *end;
  DWORD prot;
  if (GetEnvironmentVariableA("EE_X87T", spec, sizeof spec) <= 0)
    lstrcpyA(spec, "1");
  if (spec[0] == '0')
    return;
  code = (unsigned char *)VirtualAlloc(NULL, 512 * 1024, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!code)
    return;
  p = code;
  end = code + 512 * 1024;
  for (i = 0; exe_fns && exe_fns[i].addr; i++) {
    const struct x87t_fn *f = &exe_fns[i];
    unsigned char *fn = (unsigned char *)(ULONG_PTR)f->addr;
    char hex[16];
    if (strcmp(spec, "1")) {
      wsprintfA(hex, "%lx", (unsigned long)f->addr);
      if (!strstr(spec, hex))
        continue;
    }
    total++;
    if (IsBadReadPtr(fn, f->size) || x87t_fnv(fn, f->size) != f->fnv) {
      logf("x87 translation: %s (0x%lx) is not the analysed code -- left on x87", f->name, (unsigned long)f->addr);
      continue;
    }
    if (x87t_one(fn, f->size, f->prologue, f->pcall, f->recipe, f->single, &p, end, &bytes))
      done++;
    else
      logf("x87 translation: %s (0x%lx): recipe did not replay -- left on x87", f->name, (unsigned long)f->addr);
  }
  for (t = 0; lle && t < sizeof x87t_dll_tables / sizeof x87t_dll_tables[0]; t++)
    for (i = 0; x87t_dll_tables[t][i].export; i++) {
      const struct x87t_dllfn *f = &x87t_dll_tables[t][i];
      unsigned char *fn = (unsigned char *)GetProcAddress(lle, f->export);
      if (strcmp(spec, "1") && !strstr(spec, f->export))
        continue;
      total++;
      if (!fn || IsBadReadPtr(fn, f->size) || x87t_dllhash((const unsigned char *)lle, fn, f->size, f->recipe) != f->fnv) {
        logf("x87 translation: %s is not the analysed code -- left on x87", f->export);
        continue;
      }
      if (x87t_one(fn, f->size, f->prologue, f->pcall, f->recipe, f->single, &p, end, &bytes))
        done++;
      else
        logf("x87 translation: %s: recipe did not replay -- left on x87", f->export);
    }
  {
    char b[8];
    if (lle && GetEnvironmentVariableA("EE_X87T_SHADOW", b, sizeof b) > 0 && b[0] == '1')
      logf("x87 translation: shadow check on %u render functions (EE_X87T_SHADOW)",
           x87t_shadow_install(lle, &p, end, logf));
  }
  VirtualProtect(code, 512 * 1024, PAGE_EXECUTE_READ, &prot);
  FlushInstructionCache(GetCurrentProcess(), NULL, 0);
  logf("x87 translation: %u of %u game and engine functions on SSE2 (%u bytes of code)", done, total, bytes);
}
