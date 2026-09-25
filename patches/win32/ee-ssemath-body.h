/* One precision variant of the SSE maths (included twice by ee-ssemath.h, with
 * T = double for x87 at 53-bit precision and T = float for 24-bit).
 *
 * Each function is Low-Level Engine.dll's x87 code, operation for operation:
 * same operand order, same association, a (float) at every fstps, re-reads
 * after every store (callers pass aliased in/out), so the results are the
 * original's bit for bit.  Offsets in comments are the x87 code's. */

/* ?Apply@GETransformation@@QBEXABVGE3DPoint@@AAV2@@Z  (rva 0x4ab4) */
SSEM_FN static void SSEM_THIS SSEM(apply_p3)(const float *m, const float *in, float *out) {
  out[0] = (float)((((T)in[2] * m[2] + (T)in[1] * m[1]) + (T)in[0] * m[0]) + m[3]);
  out[1] = (float)((((T)in[0] * m[4] + (T)in[2] * m[6]) + (T)in[1] * m[5]) + m[7]);
  out[2] = (float)((((T)in[0] * m[8] + (T)in[2] * m[10]) + (T)in[1] * m[9]) + m[11]);
}

/* ?Apply@GETransformation@@QBEXABVGEVector@@AAV2@@Z  (rva 0x4b0e): no translation */
SSEM_FN static void SSEM_THIS SSEM(apply_v3)(const float *m, const float *in, float *out) {
  out[0] = (float)(((T)in[2] * m[2] + (T)in[1] * m[1]) + (T)in[0] * m[0]);
  out[1] = (float)(((T)in[0] * m[4] + (T)in[2] * m[6]) + (T)in[1] * m[5]);
  out[2] = (float)(((T)in[0] * m[8] + (T)in[2] * m[10]) + (T)in[1] * m[9]);
}

/* ?Apply@GETransformation@@QBEXABVGE2DPoint@@AAV2@@Z  (rva 0x4a85) */
SSEM_FN static void SSEM_THIS SSEM(apply_p2)(const float *m, const float *in, float *out) {
  out[0] = (float)(((T)in[1] * m[1] + (T)in[0] * m[0]) + m[3]);
  out[1] = (float)(((T)in[0] * m[4] + (T)in[1] * m[5]) + m[7]);
}

/* ?Apply@GETransformation@@QBEXABV1@AAV1@@Z  (rva 0x468a9): out = this * b.
 * The original copies both matrices before writing, so out may alias either. */
SSEM_FN static void SSEM_THIS SSEM(apply_xf)(const float *a, const float *b, float *out) {
  const T a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3], a4 = a[4], a5 = a[5], a6 = a[6], a7 = a[7];
  const T a8 = a[8], a9 = a[9], a10 = a[10], a11 = a[11];
  const T b0 = b[0], b1 = b[1], b2 = b[2], b3 = b[3], b4 = b[4], b5 = b[5], b6 = b[6], b7 = b[7];
  const T b8 = b[8], b9 = b[9], b10 = b[10], b11 = b[11];
  out[0] = (float)((b8 * a2 + b4 * a1) + b0 * a0);
  out[1] = (float)((b9 * a2 + b5 * a1) + b1 * a0);
  out[2] = (float)((b10 * a2 + b6 * a1) + b2 * a0);
  out[3] = (float)(((b11 * a2 + b7 * a1) + b3 * a0) + a3);
  out[4] = (float)((b8 * a6 + b4 * a5) + b0 * a4);
  out[5] = (float)((b9 * a6 + b5 * a5) + b1 * a4);
  out[6] = (float)((b10 * a6 + b6 * a5) + b2 * a4);
  out[7] = (float)(((b11 * a6 + b7 * a5) + b3 * a4) + a7);
  out[8] = (float)((b8 * a10 + b4 * a9) + b0 * a8);
  out[9] = (float)((b9 * a10 + b5 * a9) + b1 * a8);
  out[10] = (float)((b10 * a10 + b6 * a9) + b2 * a8);
  out[11] = (float)(((b11 * a10 + b7 * a9) + b3 * a8) + a11);
  memcpy(out + 12, b + 12, 4 * sizeof(float)); /* integer moves in the original */
}

/* ?CrossProduct@GEVector@@QBEXABV1@AAV1@@Z  (rva 0x19d0): out = this x b */
SSEM_FN static void SSEM_THIS SSEM(cross)(const float *a, const float *b, float *out) {
  out[0] = (float)((T)a[1] * b[2] - (T)b[1] * a[2]);
  out[1] = (float)((T)b[0] * a[2] - (T)a[0] * b[2]);
  out[2] = (float)((T)b[1] * a[0] - (T)b[0] * a[1]);
}

/* ?DistanceToSquared@GE3DPoint@@QBEMABV1@@Z  (rva 0x1dac).  The x87 result is
 * returned unrounded in st(0), so this returns T, not float. */
SSEM_FN static T SSEM_THIS SSEM(dist2)(const float *a, const float *b) {
  const T dz = (T)a[2] - b[2], dy = (T)a[1] - b[1], dx = (T)a[0] - b[0];
  return (dx * dx + dy * dy) + dz * dz;
}

/* GE3DPlane = { point p[3], normal n[3] }: the signed distance's numerator. */
SSEM_FN static T SSEM(plane_dot)(const float *pl, const float *pt) {
  const T dx = (T)pt[0] - pl[0], dy = (T)pt[1] - pl[1], dz = (T)pt[2] - pl[2];
  return (dz * pl[5] + dy * pl[4]) + dx * pl[3];
}

/* ?IsPointOnOrAbovePlane@GE3DPlane@@QBE_NABVGE3DPoint@@@Z  (rva 0x25ec8):
 * jb after fcomps 0.0 -> false below zero and for NaN. */
SSEM_FN static int SSEM_THIS SSEM(above)(const float *pl, const float *pt) {
  return SSEM(plane_dot)(pl, pt) >= 0;
}

/* ?IsPointOnOrBelowPlane@GE3DPlane@@QBE_NABVGE3DPoint@@@Z  (rva 0x25f0f):
 * ja -> false above zero; NaN counts as below. */
SSEM_FN static int SSEM_THIS SSEM(below)(const float *pl, const float *pt) {
  return !(SSEM(plane_dot)(pl, pt) > 0);
}

/* ?UMCos@@YAMM@Z  (rva 0x7df28): wrap into [-pi, pi] in 2*pi steps (the value
 * stays in the FPU register between steps), fold to |x|, scale, round to a
 * float, fistp (round to nearest) and look the table up. */
SSEM_FN static float SSEM(umcos)(float a) {
  T x = a;
  while (!(x >= (T)g_ssem.cos_lo))
    x = x + (T)g_ssem.cos_step;
  while (x > (T)g_ssem.cos_hi)
    x = x - (T)g_ssem.cos_step;
  if (!(x >= 0))
    x = -x;
  {
    const float t = (float)((T)g_ssem.cos_scale * x);
    return g_ssem.cos_table[ssem_fistp(t)];
  }
}

/* ?ComputeDimensions@GEBoundingBox@@AAEXXZ  (rva 0x26a90): order each axis's
 * min/max (swap unless max >= min), then the extents x at +0x18, z at +0x1c,
 * y at +0x20.  The swap moves raw bits, as flds/fstps do for real numbers. */
SSEM_FN static void SSEM_THIS SSEM(bbox_dims)(float *b) {
  int k;
  for (k = 0; k < 3; k++)
    if (!(b[3 + k] >= b[k])) {
      unsigned t, u;
      memcpy(&t, &b[k], 4);
      memcpy(&u, &b[3 + k], 4);
      memcpy(&b[k], &u, 4);
      memcpy(&b[3 + k], &t, 4);
    }
  b[6] = (float)((T)b[3] - b[0]);
  b[7] = (float)((T)b[5] - b[2]);
  b[8] = (float)((T)b[4] - b[1]);
}

/* ?SetOrientationYPR@GETransformation@@QAEXMMM@Z  (rva 0x472d3): the 3x3
 * rotation from yaw a, pitch b, roll c with UMCos (sin x = UMCos(pi/2 - x),
 * the argument rounded to float), leaving the translation and row 3 alone.
 * p = (-sb)(-sa) stays unrounded on the x87 stack; t1 = (-sb)ca is stored. */
SSEM_FN static void SSEM_THIS SSEM(ypr)(float *m, float a, float b, float c) {
  const T K = g_ssem.half_pi;
  const float ca = SSEM(umcos)(a), sa = SSEM(umcos)((float)(K - a)), nsa = -sa;
  const float cb = SSEM(umcos)(b), sb = SSEM(umcos)((float)(K - b)), nsb = -sb;
  const float cc = SSEM(umcos)(c), sc = SSEM(umcos)((float)(K - c)), nsc = -sc;
  const T p = (T)nsb * nsa;
  const float t1 = (float)((T)nsb * ca);
  memcpy(&m[9], &sb, 4); /* an integer move in the original */
  m[0] = (float)(p * sc + (T)cc * ca);
  m[1] = (float)((T)cb * nsa);
  m[2] = (float)(p * cc + (T)nsc * ca);
  m[4] = (float)((T)t1 * sc + (T)cc * sa);
  m[5] = (float)((T)cb * ca);
  m[6] = (float)((T)t1 * cc + (T)nsc * sa);
  m[8] = (float)((T)sc * cb);
  m[10] = (float)((T)cc * cb);
}
