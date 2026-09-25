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
  unsigned n = 0;
  /* Headings that a long match never wraps reach UMCos as angles in the
   * thousands, and the loops below step 2 pi per iteration.  In double every
   * step is exact while |a| < 2^30 (a float beyond 64 and the float 2 pi are
   * both multiples of 2^-21), so the loops end at a +- k 2 pi for the first k
   * that lands in range, and most of the way can be jumped in one exact step
   * (k 2 pi has at most 28 + 24 bits).  The jump stops at least one step short
   * -- the rounded quotient is off by far less than one -- and the loops take
   * the last steps with the original's own comparisons.  In float the steps
   * round (24-bit x87 did too), so the float variant always walks. */
  if (sizeof(T) == 8 && (x < -64 || x > 64) && x > -0x1p30 && x < 0x1p30) {
    const T st = g_ssem.cos_step;
    int k;
    if (x < 0) {
      k = (int)(((T)g_ssem.cos_lo - x) / st) - 1;
      x = x + (T)k * st;
    } else {
      k = (int)((x - (T)g_ssem.cos_hi) / st) - 1;
      x = x - (T)k * st;
    }
    n += (unsigned)k;
  }
  while (!(x >= (T)g_ssem.cos_lo))
    x = x + (T)g_ssem.cos_step, n++;
  while (x > (T)g_ssem.cos_hi)
    x = x - (T)g_ssem.cos_step, n++;
  if (g_ssem_st.on) {
    g_ssem_st.calls++;
    g_ssem_st.steps += n;
    g_ssem_st.big += n > 64;
    if ((a < 0 ? -a : a) > g_ssem_st.max)
      g_ssem_st.max = a < 0 ? -a : a;
  }
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

/* ?Intersects@GE3DLine@@QBE_NABVGE3DPlane@@AAM@Z  (rva 0x25b07): the line
 * (origin +0, direction +0xc) against the plane (point, normal): false when
 * d.n is 0 or NaN (je after fcoms 0.0), else t = (w.n) / (d.n) with w the
 * plane point minus the origin. */
SSEM_FN static int SSEM_THIS SSEM(line_plane)(const float *L, const float *pl, float *t) {
  const T wx = (T)pl[0] - L[0], wy = (T)pl[1] - L[1], wz = (T)pl[2] - L[2];
  const T den = ((T)L[5] * pl[5] + (T)L[4] * pl[4]) + (T)L[3] * pl[3];
  if (den == 0 || den != den)
    return 0;
  *t = (float)(((wz * pl[5] + wy * pl[4]) + wx * pl[3]) / den);
  return 1;
}

/* ?ComputeDirection@GE3DLine@@AAEXABVGE3DPoint@@@Z  (rva 0x1fb6): direction
 * = q - origin, one component at a time (re-reads after each store). */
SSEM_FN static void SSEM_THIS SSEM(line_dir)(float *L, const float *q) {
  L[3] = (float)((T)q[0] - L[0]);
  L[4] = (float)((T)q[1] - L[1]);
  L[5] = (float)((T)q[2] - L[2]);
}

/* DX7HRTnLDisplay.dll ?DrawTerrainMaterial@DX7Rasterizer@@... vertex loop
 * (rva 0x688d-0x6ac0): for every terrain polygon in the NULL-terminated list,
 * three 36-byte vertices: position (copied), a zero normal, a diffuse colour
 * packed from four fistp'd values (alpha*255, then r, g, b times brightness
 * times 255, each rounded to float first as the original's fstps does), and
 * the polygon's texture coordinates.  About 30 x87 instructions a vertex in
 * the original -- 11% of the render thread in a big match. */
SSEM_FN static void SSEM(tfill)(const char *const *polys, char *out) {
  const T K = g_ssem.terrain_k;
  for (; *polys; polys++) {
    const char *P = *polys;
    const float *alpha = (const float *)(P + 0xc);
    int k;
    for (k = 0; k < 3; k++) {
      const float *v = *(const float *const *)(P + 4 * k);
      float *o = (float *)out;
      unsigned c;
      int a, r, g, b;
      memcpy(o, v, 12);
      o[3] = o[4] = o[5] = 0;
      a = ssem_fistp((float)((T)alpha[k] * K));
      r = ssem_fistp((float)(((T)v[3] * v[6]) * K));
      g = ssem_fistp((float)(((T)v[4] * v[6]) * K));
      b = ssem_fistp((float)(((T)v[5] * v[6]) * K));
      c = (unsigned)a;
      c = (c << 8) | (unsigned)r;
      c = (c << 8) | (unsigned)g;
      c = (c << 8) | (unsigned)b;
      memcpy(o + 6, &c, 4);
      memcpy(o + 7, P + 0x18 + 8 * k, 8);
      out += 36;
    }
  }
}

/* DX7Rasterizer::CopyAndSmoothStdVertices / CopyAndSmoothStdColorVertices
 * (DX7HRTnLDisplay.dll rva 0x3d1e / 0x3c4f, "Animation Smoothing"): blend the
 * model's two keyframes into the vertex buffer.  t = model[0x130]; keyframe
 * vertex arrays at +4 of model[0x104][model[0x134]] (A) and [model[0x138]] (B);
 * the material's vertices start at mat[0x34], mat[0x38] of them.  Position and
 * normal: each of the six = s*A + t*B with s = 1 - t, each product rounded,
 * then the sum, then stored as a float (the order of the two products does not
 * matter: rounded sums commute).  Std input is 32 bytes (x,y,z,nx,ny,nz,u,v):
 * colour white, u,v from A.  Colour input is 36 bytes (...,colour,u,v): colour
 * and u,v from A.  Output 36 bytes either way. */
SSEM_FN static void SSEM(smooth)(const char *model, const char *mat, char *out, int color) {
  const float t = *(const float *)(model + 0x130);
  const T s = (T)1 - t;
  const char *const *frames = *(const char *const *const *)(model + 0x104);
  const unsigned stride = color ? 36 : 32;
  const unsigned first = *(const unsigned *)(mat + 0x34), count = *(const unsigned *)(mat + 0x38);
  const char *a = *(const char *const *)(frames[*(const int *)(model + 0x134)] + 4) + first * stride;
  const char *b = *(const char *const *)(frames[*(const int *)(model + 0x138)] + 4) + first * stride;
  const char *const end = a + count * stride;
  for (; a < end; a += stride, b += stride, out += 36) {
    const float *fa = (const float *)a, *fb = (const float *)b;
    float *o = (float *)out;
    int k;
    for (k = 0; k < 6; k++)
      o[k] = (float)(s * fa[k] + (T)t * fb[k]);
    if (color)
      memcpy(o + 6, fa + 6, 12);
    else {
      static const unsigned white = 0xffffffffu;
      memcpy(o + 6, &white, 4);
      memcpy(o + 7, fa + 6, 8);
    }
  }
}

/* ?GetMeshZ@GETerrainMesh@@QBEMMM@Z (rva 0x3b6ac): the terrain height at x,y,
 * interpolated over the grid triangle the point falls in.  Floor as the VC6
 * code does it: fistp, then one less if the unsigned 32-bit value is above the
 * float (a negative coordinate stays off the map, as in the original).  Rows
 * of 12 bytes {first column, -, float heights, 3 per column} hang off
 * this[0x17b578].  gx and the three products stored on the way are floats;
 * the result is returned unrounded, like the x87's st(0). */
SSEM_FN static T SSEM_THIS SSEM(meshz)(const char *mesh, float x, float y) {
  int ix = ssem_fistp(x), iy = ssem_fistp(y);
  const char *r0;
  const float *d0, *d1;
  int c0, c1;
  if ((double)(unsigned)ix > x)
    ix--;
  if ((double)(unsigned)iy > y)
    iy--;
  r0 = *(const char *const *)(mesh + 0x17b578) + iy * 12;
  d0 = *(const float *const *)(r0 + 8);
  d1 = *(const float *const *)(r0 + 0x14);
  c0 = ix - *(const int *)r0;
  c1 = ix - *(const int *)(r0 + 0xc);
  {
    const T fx = (T)((double)x - (double)(unsigned)ix), fy = (T)((double)y - (double)(unsigned)iy);
    const float gx = (float)((T)1 - fx);
    const T p = (T)d0[c0 * 3] * gx, q = (T)d1[c1 * 3 + 3] * fx, s = q + p;
    const float tc = (float)p, ys = (float)q, t4 = (float)s, t8 = d0[c0 * 3 + 3];
    if (fy >= fx) {
      const T u = (fy - fx) / gx;
      return ((T)d1[c1 * 3] * gx + ys) * u + ((T)1 - u) * t4;
    } else {
      const T v = fy / fx;
      return ((T)t8 * fx + tc) * ((T)1 - v) + v * t4;
    }
  }
}

/* ?UMArcCos@@YAMM@Z (rva 0x7dcc4): acos from a 32768-entry table below 0.9995,
 * above that atan(z), z = sqrt(1 - x*x)/x, by its series to z^7 (w = z*z, the
 * series' first difference and w stored as floats on the way); 0 from 1 up,
 * pi/2 at 0, and pi - acos(-x) below 0 with x clamped at -1 (NaN too).  The
 * square root is the DLL's own import (msvcrt), called as the x87 code calls
 * it.  Returns unrounded, like the x87's st(0). */
SSEM_FN static T SSEM(umacos)(float x) {
  if (x > 0) {
    if (x < g_ssem.ac_lim)
      return g_ssem.ac_table[ssem_fistp((float)((T)x * g_ssem.ac_scale))];
    if (x < 1.0f) {
      const T d = (T)1 - (T)x * x;
      const double s = g_ssem.crt_sqrt((double)d);
      const T z = (T)(s / x);
      const float w = (float)(z * z), zf = (float)z;
      const T zw = z * w;
      const float A = (float)((T)zf - (T)g_ssem.ac_c3 * zw);
      const T zww = zw * w;
      return ((T)g_ssem.ac_c4 * zww + A) - ((T)w * zww) * g_ssem.ac_c5;
    }
    return 0;
  }
  if (x >= 0)
    return g_ssem.half_pi;
  if (!(x >= g_ssem.ac_clamp))
    x = g_ssem.ac_clamp;
  return (T)g_ssem.ac_pi - SSEM(umacos)(-x);
}

/* ?UMArcTan2@@YAMMM@Z (rva 0x7debb): atan2(a, b) as +-UMArcCos(b / |(a,b)|),
 * negative when a < 0 (or NaN); b zero or NaN gives +-pi/2 by a's sign. */
SSEM_FN static T SSEM(umatan2)(float a, float b) {
  if (b == 0 || b != b)
    return a >= 0 ? g_ssem.half_pi : g_ssem.ac_nhalf_pi;
  {
    const T r2 = (T)a * a + (T)b * b;
    const double s = g_ssem.crt_sqrt((double)r2);
    const float c = (float)(T)(b / s);
    const T r = SSEM(umacos)(c);
    return (a < 0 || a != a) ? -r : r;
  }
}

/* ?Intersects@GE3DLine@@QBE_NABVGE3DPoint@@MAAM@Z (rva 0x2568d): the line
 * (origin L, direction L+3) against a sphere (centre P, radius r): the
 * quadratic's terms as the x87 code forms them (v = L - P, a = |dir|^2 and b
 * stored as floats; c from the expanded squares, as written; the
 * discriminant stored as a float).  No root: false.  One: t = -b/2a.  Two: t
 * is the one of smaller magnitude (the first unless the second is smaller). */
SSEM_FN static int SSEM_THIS SSEM(line_sphere)(const float *L, const float *P, float r, float *t) {
  const float vx = (float)((T)L[0] - P[0]), vy = (float)((T)L[1] - P[1]), vz = (float)((T)L[2] - P[2]);
  const float a = (float)(((T)L[3] * L[3] + (T)L[4] * L[4]) + (T)L[5] * L[5]);
  const float b = (float)((((T)L[5] * vz + (T)L[4] * vy) + (T)L[3] * vx) * 2);
  const T s5 = (((((T)L[0] * L[0] + (T)P[0] * P[0]) + (T)L[1] * L[1]) + (T)L[2] * L[2]) + (T)P[1] * P[1]) + (T)P[2] * P[2];
  const T d3 = ((T)P[0] * L[0] + (T)P[1] * L[1]) + (T)P[2] * L[2];
  const T c = (s5 - d3 * 2) - (T)r * r;
  const float disc = (float)((T)b * b - (c * a) * 4);
  if (!(disc >= 0))
    return 0;
  if (disc > 0) {
    const T s = (T)g_ssem.crt_sqrt((double)disc), k = (T)1 / ((T)a + a);
    const float t1 = (float)((s - b) * k);
    const T t2 = (-(T)b - s) * k;
    const float a1 = t1 >= 0 ? t1 : -t1;
    const T a2 = t2 >= 0 ? t2 : -t2;
    *t = a1 >= a2 ? (float)t2 : t1;
    return 1;
  }
  *t = (float)-((T)b / ((T)a + a));
  return 1;
}

/* ?NormalizePerspectiveCameraPoint@GEViewport@@QAEXABVGE3DPoint@@AAV2@AAM@Z
 * (rva 0x5e0c): out = (in.x*s, in.z*s, in.y*k + c) with s, k, c at viewport
 * +0x248/+0x24c/+0x250, then *w = in.y.  Callers pass out == in: each input is
 * read after the store before it, as the x87 does.  *w went through an x87
 * load and store, which quiets a signalling NaN. */
SSEM_FN static void SSEM_THIS SSEM(norm_cam)(const char *vp, const float *in, float *out, float *w) {
  const float s = *(const float *)(vp + 0x248);
  unsigned u;
  out[0] = (float)((T)in[0] * s);
  out[1] = (float)((T)in[2] * *(const float *)(vp + 0x248));
  out[2] = (float)((T)in[1] * *(const float *)(vp + 0x24c) + *(const float *)(vp + 0x250));
  memcpy(&u, &in[1], 4);
  if ((u & 0x7f800000u) == 0x7f800000u && (u & 0x003fffffu) && !(u & 0x00400000u))
    u |= 0x00400000u;
  memcpy(w, &u, 4);
}

/* ?NormalizeOrthographicCameraPoint@GEViewport@@QAEXABVGE3DPoint@@AAV2@AAM@Z
 * (rva 0x5dd1): the orthographic twin, with s, k, c at +0x23c/+0x240/+0x244
 * and *w = 1. */
SSEM_FN static void SSEM_THIS SSEM(norm_ortho)(const char *vp, const float *in, float *out, float *w) {
  out[0] = (float)((T)in[0] * *(const float *)(vp + 0x23c));
  out[1] = (float)((T)in[2] * *(const float *)(vp + 0x23c));
  out[2] = (float)((T)in[1] * *(const float *)(vp + 0x240) + *(const float *)(vp + 0x244));
  *w = 1.0f;
}

/* ?GetPixelSize@GERasterizer@@QAEMPAVGEModel@@PAVGEViewport@@AAVGETransformation@@M_N@Z
 * (rva 0x3531d): the model's size on screen.  Its centre (model+0xc8) goes
 * through the model's transform and the viewport's view transform (+0xf0),
 * then x - r and x + r (r = the model's first radius, model[0x124][0], times
 * scale) are projected, perspective or orthographic.  The width times the
 * viewport's factor (+0x26c), or twice the factor when it comes out negative.
 * A perspective w of zero (or NaN: the x87's je) counts as zero. */
SSEM_FN static T SSEM_THIS SSEM(pixel_size)(const char *model, const char *vp, const float *xf, float scale, unsigned ortho) {
  float A[3] = {0, 0, 0}, B[3] = {0, 0, 0}, w, ax, rsf, t1f;
  const float f = *(const float *)(vp + 0x26c);
  T rs, diff;
  SSEM(apply_p3)(xf, (const float *)(model + 0xc8), B);
  SSEM(apply_p3)((const float *)(vp + 0xf0), B, A);
  rs = (T)**(const float *const *)(model + 0x124) * scale;
  ax = A[0];
  rsf = (float)rs;
  A[0] = (float)((T)A[0] - rs);
  if (!(ortho & 0xff)) {
    SSEM(norm_cam)(vp, A, B, &w);
    t1f = (w == 0 || w != w) ? 0.0f : (float)((T)B[0] / w);
    A[0] = (float)((T)ax + rsf);
    SSEM(norm_cam)(vp, A, B, &w);
    diff = (w == 0 || w != w) ? (T)0 : (T)B[0] / w - t1f;
  } else {
    SSEM(norm_ortho)(vp, A, B, &w);
    t1f = B[0];
    A[0] = (float)((T)ax + rsf);
    SSEM(norm_ortho)(vp, A, B, &w);
    diff = (T)B[0] - t1f;
  }
  if (diff < 0 || diff != diff)
    return (T)f + f;
  return diff * f;
}

/* ?PrepareTransforms@GEModel@@QAEXXZ (rva 0x317f1): the model's world matrix
 * (+0x88) = its parent's (+0x48) x (its animation frame's (+0xf0 array, 64
 * bytes each, index +0x134) x its local one (+8)), the 3x3 part of the middle
 * product scaled by +0x128, then the same for each child (+0xfc, count
 * +0x13c).  The translation column is copied through the x87 (quieting a
 * signalling NaN); the bottom row becomes 0 0 0 1. */
SSEM_FN static void SSEM_THIS SSEM(prep_xf)(char *model) {
  const float s = *(const float *)(model + 0x128);
  const unsigned nch = *(const unsigned *)(model + 0x13c);
  float tmp[16], sc[16];
  unsigned i;
  SSEM(apply_xf)((const float *)(*(const char *const *)(model + 0xf0) + (*(const int *)(model + 0x134) << 6)),
                 (const float *)(model + 8), tmp);
  for (i = 0; i < 3; i++) {
    sc[4 * i] = (float)((T)tmp[4 * i] * s);
    sc[4 * i + 1] = (float)((T)tmp[4 * i + 1] * s);
    sc[4 * i + 2] = (float)((T)tmp[4 * i + 2] * s);
    ssem_copyq(&sc[4 * i + 3], &tmp[4 * i + 3]);
  }
  sc[12] = sc[13] = sc[14] = 0;
  sc[15] = 1;
  SSEM(apply_xf)((const float *)(model + 0x48), sc, (float *)(model + 0x88));
  for (i = 0; i < nch; i++)
    SSEM(prep_xf)((*(char *const *const *)(model + 0xfc))[i]);
}

/* ?IsPerspectiveModelVisible@GERasterizer@@IAEXPAVGEModel@@MPAVGEViewport@@AAVGETransformation@@AA_N3@Z
 * (rva 0x35772): frustum culling.  The model's centre (+0xc8) goes into camera
 * space (its transform, then the viewport's +0xf0); the four side planes'
 * normals (viewport +0x50/+0x68/+0x80/+0x98, normal at +0xc) are scaled by
 * the radius r = model[0x124][model[0x134]] * scale and stored as floats.
 * Out if the near distance (+0x264) is beyond y + r or the far one (+0x270)
 * short of y - r; out unless centre + each normal is on or above its plane.
 * Visible: flags in the model for near (+0x158), far (+0x159) and each plane
 * the model crosses (centre - normal on or below it: +0x15c, +0x15d, +0x15a,
 * +0x15b), and *clipped if any is set.  The flags use r stored as a float. */
SSEM_FN static void SSEM_THIS SSEM(persp_vis)(unsigned char *model, float scale, const char *vp, const float *xf,
                                              unsigned char *visible, unsigned char *clipped) {
  static const unsigned short poff[4] = {0x50, 0x68, 0x80, 0x98}, foff[4] = {0x15c, 0x15d, 0x15a, 0x15b};
  float P[3] = {0, 0, 0}, Q[3] = {0, 0, 0}, N[4][3], rf;
  T r;
  int i, k, sum;
  for (i = 0; i < 4; i++)
    memcpy(N[i], vp + poff[i] + 0xc, 12);
  *visible = 0;
  *clipped = 0;
  SSEM(apply_p3)(xf, (const float *)(model + 0xc8), Q);
  SSEM(apply_p3)((const float *)(vp + 0xf0), Q, P);
  r = (T)(*(const float *const *)(model + 0x124))[*(const int *)(model + 0x134)] * scale;
  rf = (float)r;
  for (i = 0; i < 4; i++)
    for (k = 0; k < 3; k++)
      N[i][k] = (float)((T)N[i][k] * r);
  if (*(const float *)(vp + 0x264) > (T)P[1] + r)
    return;
  if (!(*(const float *)(vp + 0x270) >= (T)P[1] - r))
    return;
  for (i = 0; i < 4; i++) {
    Q[0] = (float)((T)N[i][0] + P[0]);
    Q[1] = (float)((T)N[i][1] + P[1]);
    Q[2] = (float)((T)N[i][2] + P[2]);
    if (!SSEM(above)((const float *)(vp + poff[i]), Q))
      return;
  }
  *visible = 1;
  model[0x158] = *(const float *)(vp + 0x264) > (T)P[1] - rf;
  model[0x159] = !(*(const float *)(vp + 0x270) >= (T)P[1] + rf);
  sum = model[0x158] + model[0x159];
  for (i = 0; i < 4; i++) {
    Q[0] = (float)((T)P[0] - N[i][0]);
    Q[1] = (float)((T)P[1] - N[i][1]);
    Q[2] = (float)((T)P[2] - N[i][2]);
    model[foff[i]] = (unsigned char)SSEM(below)((const float *)(vp + poff[i]), Q);
    sum += model[foff[i]];
  }
  *clipped = sum != 0;
}
