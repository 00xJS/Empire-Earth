/* ---- call-site timing (EE_CALLTIME, diagnostics) ----------------------------
 * Exact inclusive time of chosen calls in the game, to find what a simulation
 * tick spends its ~11 ms on.  A sampling profiler cannot be trusted with
 * Rosetta's translated x87 code: it put 73% of the render thread in lock waits
 * that the lock statistics measure at 20% (25 Sep 2026).
 *
 * EE_CALLTIME=<hex addresses of call instructions in Empire Earth.exe>, comma-
 * separated: "call rel32" (E8) or "call [abs32]" (FF 15).  Each one is
 * re-pointed at a generated thunk that drops its own return address -- the
 * callee sees exactly the stack it would have -- calls the original target
 * between two rdtsc reads, and jumps back after the call instruction.  No
 * return address is ever swapped, so a longjmp through a timed call only
 * spoils one sample.  Only the thread given to ct_set_thread() is timed (the
 * world-lock writer, i.e. the simulation); EE_CALLTIME_ALL=1 times every thread.
 * Installed from DllMain, before any game thread runs.  The helpers the thunks
 * call use integers only: a timed function may return a float in st(0). */
#define CT_MAX 48
static struct ct_site {
  unsigned char *at, *target, *slot; /* call site; E8 target, or the copied instruction */
  int indirect;                      /* call [reg+disp32]: time per actual target too */
  volatile LONG n;
  volatile LONGLONG cyc;
  struct { void *volatile t; volatile LONG n; volatile LONGLONG cyc; } tt[32];
} ct_sites[CT_MAX];
static int ct_n, ct_all;
static volatile DWORD ct_tid;
static DWORD ct_tls = TLS_OUT_OF_INDEXES;
static volatile LONG ct_bad;
static LONGLONG ct_q0, ct_c0; /* QPC and rdtsc at the last report, for the rate */

struct ct_stack { int sp; int site[64]; void *tgt[64]; unsigned long long t0[64]; };

static inline DWORD ct_self(void) {
  DWORD t;
  __asm__ volatile("movl %%fs:0x24, %0" : "=r"(t));
  return t;
}

/* The thread's slot straight from the TEB (TlsSlots at fs:0xe10; TlsAlloc
 * hands out indexes under 64 first): TlsGetValue would reset the thread's
 * last-error value under the game's feet. */
static inline struct ct_stack *ct_get(void) {
  struct ct_stack *s;
  __asm__ volatile("movl %%fs:0xe10(,%1,4), %0" : "=r"(s) : "r"(ct_tls));
  return s;
}
static struct ct_stack *ct_stack(void) {
  struct ct_stack *s = ct_get();
  if (!s) {
    DWORD e = GetLastError();
    s = (struct ct_stack *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *s);
    TlsSetValue(ct_tls, s);
    SetLastError(e);
  }
  return s;
}

static void __cdecl ct_begin(int i, void *tgt) {
  struct ct_stack *s;
  if (!ct_all && ct_self() != ct_tid)
    return;
  s = ct_stack();
  if (!s)
    return;
  if (s->sp < 64) {
    s->site[s->sp] = i;
    s->tgt[s->sp] = tgt;
    s->t0[s->sp] = __builtin_ia32_rdtsc();
  }
  s->sp++;
}

static void __cdecl ct_end(int i) {
  unsigned long long t = __builtin_ia32_rdtsc();
  struct ct_stack *s;
  if (!ct_all && ct_self() != ct_tid)
    return;
  s = ct_get();
  if (!s || s->sp <= 0)
    return;
  s->sp--;
  if (s->sp < 64 && s->site[s->sp] == i) {
    const LONGLONG d = (LONGLONG)(t - s->t0[s->sp]);
    struct ct_site *c = &ct_sites[i];
    InterlockedIncrement(&c->n);
    InterlockedExchangeAdd64(&c->cyc, d);
    if (c->indirect) {
      void *tg = s->tgt[s->sp];
      int k;
      for (k = 0; k < 32; k++)
        if (c->tt[k].t == tg || (!c->tt[k].t && InterlockedCompareExchangePointer((void *volatile *)&c->tt[k].t, tg, NULL) == NULL)) {
          InterlockedIncrement(&c->tt[k].n);
          InterlockedExchangeAdd64(&c->tt[k].cyc, d);
          break;
        }
    }
  } else { /* unbalanced (a longjmp, or the thread filter changed): start over */
    InterlockedIncrement(&ct_bad);
    s->sp = 0;
  }
}

static void ct_set_thread(DWORD tid) { ct_tid = tid; }

static unsigned char *ct_emit32(unsigned char *p, unsigned char op, const void *to) {
  int rel = (int)((const unsigned char *)to - (p + 5));
  *p = op;
  memcpy(p + 1, &rel, 4);
  return p + 5;
}
static unsigned char *ct_push_imm(unsigned char *p, int v) {
  *p = 0x68;
  memcpy(p + 1, &v, 4);
  return p + 5;
}

static void ct_install(void (*logf)(const char *fmt, ...)) {
  char spec[1024], b[8], *tok;
  unsigned char *code;
  DWORD old;
  if (!GetEnvironmentVariableA("EE_CALLTIME", spec, sizeof spec))
    return;
  ct_all = GetEnvironmentVariableA("EE_CALLTIME_ALL", b, sizeof b) > 0 && b[0] == '1';
  ct_tls = TlsAlloc();
  code = VirtualAlloc(NULL, CT_MAX * 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
  if (ct_tls >= 64 || !code) {
    logf("calltime: no TLS slot under 64 (%lu) or no memory -- off", (unsigned long)ct_tls);
    return;
  }
  for (tok = strtok(spec, ", "); tok && ct_n < CT_MAX; tok = strtok(NULL, ", ")) {
    unsigned char *at = (unsigned char *)strtoul(tok, NULL, 16), *p = code + ct_n * 64, *back;
    struct ct_site *c = &ct_sites[ct_n];
    int len;
    if (IsBadReadPtr(at, 6) || at < (unsigned char *)0x401000 || at >= (unsigned char *)0x823000) {
      logf("calltime: %s is not in the game's code", tok);
      continue;
    }
    if (at[0] == 0xE8) {
      int rel;
      memcpy(&rel, at + 1, 4);
      c->target = at + 5 + rel;
      len = 5;
    } else if (at[0] == 0xFF && (at[1] == 0x15 || ((at[1] & 0xF8) == 0x90 && at[1] != 0x94))) {
      /* call [abs32] or call [reg+disp32] (not esp-based): six position-
       * independent bytes, copied into the thunk as they are */
      c->slot = at;
      len = 6;
    } else {
      logf("calltime: %p is not a call rel32 / call [abs32] (%02x %02x)", (void *)at, at[0], at[1]);
      continue;
    }
    c->at = at;
    back = at + len;
    /* add esp,4 / push eax,ecx,edx / push the target / push i / call ct_begin /
     * add esp,8 / pop.  For call [reg+disp32] the target is pushed with the
     * same addressing (FF /6), the registers still as the caller left them. */
    c->indirect = at[0] == 0xFF && at[1] != 0x15;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;
    *p++ = 0x50; *p++ = 0x51; *p++ = 0x52;
    if (c->indirect) {
      *p++ = 0xFF; *p++ = (unsigned char)(0xB0 | (at[1] & 7));
      memcpy(p, at + 2, 4);
      p += 4;
    } else
      p = ct_push_imm(p, (int)(c->target ? c->target : 0));
    p = ct_push_imm(p, ct_n);
    p = ct_emit32(p, 0xE8, (void *)ct_begin);
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x08;
    *p++ = 0x5A; *p++ = 0x59; *p++ = 0x58;
    if (c->target) /* the original call */
      p = ct_emit32(p, 0xE8, c->target);
    else {
      memcpy(p, at, 6);
      p += 6;
    }
    /* push eax,edx / push i / call ct_end / add esp,4 / pop edx,eax / jmp back */
    *p++ = 0x50; *p++ = 0x52;
    p = ct_push_imm(p, ct_n);
    p = ct_emit32(p, 0xE8, (void *)ct_end);
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;
    *p++ = 0x5A; *p++ = 0x58;
    ct_emit32(p, 0xE9, back);
    if (!VirtualProtect(at, len, PAGE_EXECUTE_READWRITE, &old))
      continue;
    ct_emit32(at, 0xE8, code + ct_n * 64);
    if (len == 6)
      at[5] = 0x90;
    VirtualProtect(at, len, old, &old);
    ct_n++;
  }
  FlushInstructionCache(GetCurrentProcess(), NULL, 0);
  {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    ct_q0 = q.QuadPart;
    ct_c0 = (LONGLONG)__builtin_ia32_rdtsc();
  }
  logf("calltime: timing %d call sites (%s)", ct_n, ct_all ? "every thread" : "the simulation thread");
}

/* Every call site's calls per second and time, for the period since the last
 * report (secs long).  Integer maths only up to the final printf. */
static void ct_report(void (*logf)(const char *fmt, ...), double secs) {
  LARGE_INTEGER q, f;
  LONGLONG c = (LONGLONG)__builtin_ia32_rdtsc();
  double cyc_per_ms;
  int i;
  if (!ct_n)
    return;
  QueryPerformanceCounter(&q);
  QueryPerformanceFrequency(&f);
  cyc_per_ms = (double)(c - ct_c0) / ((double)(q.QuadPart - ct_q0) * 1000.0 / (double)f.QuadPart);
  ct_q0 = q.QuadPart;
  ct_c0 = c;
  for (i = 0; i < ct_n; i++) {
    LONG n = InterlockedExchange(&ct_sites[i].n, 0);
    LONGLONG cy = InterlockedExchange64(&ct_sites[i].cyc, 0);
    if (n)
      logf("  call %p -> %p: %.1f/s, %.3f ms each, %.1f ms/s", (void *)ct_sites[i].at,
           ct_sites[i].target ? (void *)ct_sites[i].target : NULL, n / secs,
           (double)cy / cyc_per_ms / n, (double)cy / cyc_per_ms / secs);
  }
  for (i = 0; i < ct_n; i++) {
    struct ct_site *c = &ct_sites[i];
    int shown;
    if (!c->indirect)
      continue;
    for (shown = 0; shown < 12; shown++) { /* the busiest targets, in order */
      int k, best = -1;
      for (k = 0; k < 32 && c->tt[k].t; k++)
        if (c->tt[k].n && (best < 0 || c->tt[k].cyc > c->tt[best].cyc))
          best = k;
      if (best < 0)
        break;
      logf("    %p via %p: %.1f/s, %.4f ms each, %.1f ms/s", (void *)c->at, c->tt[best].t, c->tt[best].n / secs,
           (double)c->tt[best].cyc / cyc_per_ms / c->tt[best].n, (double)c->tt[best].cyc / cyc_per_ms / secs);
      c->tt[best].n = 0;
      c->tt[best].cyc = 0;
    }
    { int k; for (k = 0; k < 32; k++) { c->tt[k].n = 0; c->tt[k].cyc = 0; } }
  }
  if (ct_bad)
    logf("  calltime: %ld unbalanced samples dropped", (long)InterlockedExchange(&ct_bad, 0));
}

/* ---- simulation checksum (EE_SIMHASH=1, diagnostics) ------------------------
 * A lockstep engine should reach the same world from the same save after the
 * same number of physics ticks, so a checksum of every unit after each tick
 * compares two builds: identical from tick to tick means a rewritten function
 * changed nothing.  The per-object update call in the world update
 * (0x547b5c: call 0x547e0e, ecx = the object, no stack arguments) goes through
 * a thunk that hashes the object after it has moved: its transform (16 floats
 * at +0x130), velocities (+0x230..+0x24c), state (+0x264) and id (+0x1c).
 * The world-update call (0x546d6b) closes each tick's hash; ticks are counted
 * from the first one after the save loads, and every 50th is logged. */
static volatile unsigned sh_h, sh_n;
static volatile LONG sh_tick;
static void (*sh_logf)(const char *fmt, ...);
/* Game speed: every object's ground movement (x +0x13c, y +0x14c) per tick,
 * summed and logged against wall time -- whether more ticks per second means
 * a faster game (a fixed step per tick) or only a finer one. */
static struct { const unsigned char *o; float x, y; } sh_prev[4096];
static double sh_dist;
static LARGE_INTEGER sh_t0;
static void sh_move(const unsigned char *o) {
  unsigned i = ((unsigned)(ULONG_PTR)o >> 4) & 4095, k;
  const float x = *(const float *)(o + 0x13c), y = *(const float *)(o + 0x14c);
  for (k = 0; k < 4096; k++, i = (i + 1) & 4095)
    if (sh_prev[i].o == o || !sh_prev[i].o)
      break;
  if (k == 4096)
    return;
  if (sh_prev[i].o == o) {
    const double dx = x - sh_prev[i].x, dy = y - sh_prev[i].y, d2 = dx * dx + dy * dy;
    if (d2 < 100.0) /* a teleport or a new object reusing the memory is not movement */
      sh_dist += __builtin_sqrt(d2);
  }
  sh_prev[i].o = o;
  sh_prev[i].x = x;
  sh_prev[i].y = y;
}
static void __cdecl sh_obj(const unsigned char *o) {
  unsigned h = sh_h, k;
  sh_move(o);
  static const unsigned short off[] = {0x1c, 0x264};
  for (k = 0; k < 16; k++)
    h = (h ^ *(const unsigned *)(o + 0x130 + 4 * k)) * 16777619u;
  for (k = 0; k < 8; k++)
    h = (h ^ *(const unsigned *)(o + 0x230 + 4 * k)) * 16777619u;
  for (k = 0; k < 2; k++)
    h = (h ^ *(const unsigned *)(o + off[k])) * 16777619u;
  sh_h = h;
  sh_n++;
}
static void __cdecl sh_tick_end(void) {
  LONG t = sh_tick++;
  if (t % 50 == 0 && sh_logf) {
    LARGE_INTEGER now, f;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&f);
    sh_logf("simhash: tick %ld: %08x over %u objects; moved %.1f in %.0f ms", (long)t, sh_h, sh_n, sh_dist,
            sh_t0.QuadPart ? (double)(now.QuadPart - sh_t0.QuadPart) * 1000.0 / (double)f.QuadPart : 0.0);
    sh_t0 = now;
    sh_dist = 0;
  }
  sh_h = 2166136261u;
  sh_n = 0;
}
static void sh_install(void (*logf)(const char *fmt, ...)) {
  static const unsigned char want_obj[5] = {0xE8, 0xAD, 0x02, 0x00, 0x00}; /* call 0x547e0e */
  static const unsigned char want_tick[5] = {0xE8, 0x3D, 0x0D, 0x00, 0x00}; /* call 0x547aad */
  unsigned char *obj = (unsigned char *)0x547b5c, *tick = (unsigned char *)0x546d6b, *code, *p;
  char b[8];
  DWORD old;
  if (!(GetEnvironmentVariableA("EE_SIMHASH", b, sizeof b) > 0 && b[0] == '1'))
    return;
  if (IsBadReadPtr(obj, 5) || memcmp(obj, want_obj, 5) || memcmp(tick, want_tick, 5)) {
    logf("simhash: not the reversed Empire Earth.exe -- off");
    return;
  }
  code = VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
  if (!code)
    return;
  sh_logf = logf;
  sh_h = 2166136261u;
  /* object thunk: add esp,4 / push ecx / call 0x547e0e / pop ecx /
   * push eax,edx / push ecx / call sh_obj / add esp,4 / pop edx,eax / jmp 0x547b61 */
  p = code;
  *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;
  *p++ = 0x51;
  p = ct_emit32(p, 0xE8, (void *)0x547e0e);
  *p++ = 0x59;
  *p++ = 0x50; *p++ = 0x52; *p++ = 0x51;
  p = ct_emit32(p, 0xE8, (void *)sh_obj);
  *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;
  *p++ = 0x5A; *p++ = 0x58;
  p = ct_emit32(p, 0xE9, obj + 5);
  /* tick thunk: add esp,4 / call 0x547aad / push eax,edx / call sh_tick_end /
   * pop edx,eax / jmp 0x546d70 */
  p = code + 64;
  *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;
  p = ct_emit32(p, 0xE8, (void *)0x547aad);
  *p++ = 0x50; *p++ = 0x52;
  p = ct_emit32(p, 0xE8, (void *)sh_tick_end);
  *p++ = 0x5A; *p++ = 0x58;
  p = ct_emit32(p, 0xE9, tick + 5);
  VirtualProtect(obj, 5, PAGE_EXECUTE_READWRITE, &old);
  ct_emit32(obj, 0xE8, code);
  VirtualProtect(obj, 5, old, &old);
  VirtualProtect(tick, 5, PAGE_EXECUTE_READWRITE, &old);
  ct_emit32(tick, 0xE8, code + 64);
  VirtualProtect(tick, 5, old, &old);
  FlushInstructionCache(GetCurrentProcess(), NULL, 0);
  logf("simhash: on (every object after its update, logged every 50 ticks)");
}

/* ---- physics share (EE_PHYSICS_SHARE=<percent>) ------------------------------
 * The synchronous server sets the simulation rate from how long a physics tick
 * takes: ticks/s = floor(10 * share / average tick ms), capped at 30, with
 * share = 30 -- the percentage of one CPU the simulation may use, a 2001
 * single-core budget (NESynchronousServer::TuneForSlowCPU; the value is read
 * once, when the server is made, at 0x4c253b: mov eax,[0x919424]).  Each tick
 * moves the world a fixed step, so fewer ticks per second is a slower game.
 * This makes that read a constant.  EE_SIMHASH=1 pins it at 100 (always 30
 * ticks/s) so two runs cut the same ticks into the same command turns. */
static void share_install(void (*logf)(const char *fmt, ...)) {
  static const unsigned char want[5] = {0xA1, 0x24, 0x94, 0x91, 0x00};
  unsigned char *at = (unsigned char *)0x4c253b;
  char b[16];
  int pct = 0;
  DWORD old;
  if (GetEnvironmentVariableA("EE_PHYSICS_SHARE", b, sizeof b) > 0)
    pct = atoi(b);
  else if (GetEnvironmentVariableA("EE_SIMHASH", b, sizeof b) > 0 && b[0] == '1')
    pct = 100;
  if (pct <= 0)
    return;
  if (pct > 100)
    pct = 100;
  if (IsBadReadPtr(at, 5) || memcmp(at, want, 5)) {
    logf("physics share: not the reversed Empire Earth.exe -- left at the game's own");
    return;
  }
  VirtualProtect(at, 5, PAGE_EXECUTE_READWRITE, &old);
  at[0] = 0xB8; /* mov eax, pct */
  memcpy(at + 1, &pct, 4);
  VirtualProtect(at, 5, old, &old);
  FlushInstructionCache(GetCurrentProcess(), at, 5);
  logf("physics share: %d%% of a CPU (the game's is 30%%)", pct);
}

/* ---- function timing (EE_FUNCTIME, diagnostics) -----------------------------
 * EE_FUNCTIME=<spec>,<spec>,... times whole functions, inclusive: exports of
 * Low-Level Engine.dll ("lle:<name>"), of the Direct3D renderers ("tnl:<name>",
 * hooked in each renderer as it loads) or code in the exe ("exe:<hex>").  The
 * entry jumps to a thunk that notes the caller's return address and rdtsc on a
 * per-thread shadow stack, puts its own exit thunk in as the return address
 * and runs the function from a trampoline (its first instructions, copied;
 * only position-independent prologue instructions are accepted).  Calls on the
 * render thread (the one that reads the world from 0x4feda6) are counted apart
 * from the rest.  Engine and renderer code never longjmps, which keeps the
 * shadow stack balanced; game code that might is not a safe target.  Needs
 * EE_LOCK_STATS=1 (reports ride on its 10 s lines). */
#define FT_MAX 40
static struct ft_fn {
  unsigned char *fn;
  char name[48];
  volatile LONG n[2];
  volatile LONGLONG cyc[2];
} ft_fns[FT_MAX];
static volatile LONG ft_n;
static volatile DWORD ft_render_tid;
static DWORD ft_tls = TLS_OUT_OF_INDEXES;
static LONGLONG ft_q0, ft_c0;
static char ft_spec[8192];
struct ft_stack { int sp; struct { int i; void *ret; unsigned long long t0; } e[128]; };

static inline struct ft_stack *ft_get(void) {
  struct ft_stack *s;
  __asm__ volatile("movl %%fs:0xe10(,%1,4), %0" : "=r"(s) : "r"(ft_tls));
  return s;
}
static void __cdecl ft_begin(int i, void *ret) {
  struct ft_stack *s = ft_get();
  if (!s) {
    DWORD e = GetLastError();
    s = (struct ft_stack *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *s);
    TlsSetValue(ft_tls, s);
    SetLastError(e);
  }
  if (s->sp < 128) {
    s->e[s->sp].i = i;
    s->e[s->sp].ret = ret;
    s->e[s->sp].t0 = __builtin_ia32_rdtsc();
  }
  s->sp++; /* past 128 the return address is lost: never that deep in engine code */
}
static void *__cdecl ft_end(int i) {
  const unsigned long long t = __builtin_ia32_rdtsc();
  struct ft_stack *s = ft_get();
  int k = s->sp - 1;
  while (k > 0 && s->e[k].i != i) /* skip frames left by an unwound call */
    k--;
  s->sp = k;
  {
    const int r = ct_self() == ft_render_tid ? 0 : 1;
    InterlockedIncrement(&ft_fns[i].n[r]);
    InterlockedExchangeAdd64(&ft_fns[i].cyc[r], (LONGLONG)(t - s->e[k].t0));
  }
  return s->e[k].ret;
}

/* Length of one position-independent prologue instruction, 0 if unsure. */
static int ft_modrm(const unsigned char *q) {
  const int mod = q[0] >> 6, rm = q[0] & 7;
  if (mod == 3) return 1;
  if (mod == 0) return rm == 4 ? ((q[1] & 7) == 5 ? 6 : 2) : rm == 5 ? 5 : 1;
  return (mod == 1 ? 2 : 5) + (rm == 4);
}
static int ft_len(const unsigned char *p) {
  switch (p[0]) {
  case 0x50: case 0x51: case 0x52: case 0x53: case 0x55: case 0x56: case 0x57: return 1;
  case 0x6A: return 2;
  case 0x68: case 0xA1: case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBE: case 0xBF: return 5;
  case 0x03: case 0x0B: case 0x23: case 0x2B: case 0x33: case 0x3B: case 0x85: case 0x89: case 0x8B: case 0x8D:
    return 1 + ft_modrm(p + 1);
  case 0x83: return 2 + ft_modrm(p + 1);
  case 0x81: return 5 + ft_modrm(p + 1);
  case 0xD8: case 0xD9: case 0xDC: case 0xDD: return 1 + ft_modrm(p + 1);
  case 0x64: return p[1] == 0xA1 ? 6 : 0;
  case 0xFF: return ((p[1] >> 3) & 7) == 6 ? 1 + ft_modrm(p + 1) : 0; /* push r/m32 */
  }
  return 0;
}

static void ft_hook(unsigned char *fn, const char *name, void (*logf)(const char *fmt, ...)) {
  unsigned char *code, *p, *tramp, *exitp;
  int n = 0, i;
  DWORD old;
  if (!fn || ft_n >= FT_MAX)
    return;
  while (n < 5) {
    const int l = ft_len(fn + n);
    if (!l) {
      logf("functime: %s at %p starts with %02x %02x %02x -- not hooked", name, (void *)fn, fn[n], fn[n + 1], fn[n + 2]);
      return;
    }
    n += l;
  }
  code = VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
  if (!code)
    return;
  i = ft_n;
  ft_fns[i].fn = fn;
  lstrcpynA(ft_fns[i].name, name, sizeof ft_fns[i].name);
  tramp = code + 48;
  exitp = code + 80;
  /* entry: push eax,ecx,edx / push [esp+12] / push i / call ft_begin / add esp,8 /
   * pop edx,ecx,eax / mov [esp],exit / jmp tramp */
  p = code;
  *p++ = 0x50; *p++ = 0x51; *p++ = 0x52;
  *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x0C;
  p = ct_push_imm(p, i);
  p = ct_emit32(p, 0xE8, (void *)ft_begin);
  *p++ = 0x83; *p++ = 0xC4; *p++ = 0x08;
  *p++ = 0x5A; *p++ = 0x59; *p++ = 0x58;
  *p++ = 0xC7; *p++ = 0x04; *p++ = 0x24;
  memcpy(p, &exitp, 4);
  p += 4;
  ct_emit32(p, 0xE9, tramp);
  /* trampoline: the first n bytes, then back into the function */
  memcpy(tramp, fn, n);
  ct_emit32(tramp + n, 0xE9, fn + n);
  /* exit: push eax,edx / push i / call ft_end / add esp,4 / mov ecx,eax / pop edx,eax / jmp ecx */
  p = exitp;
  *p++ = 0x50; *p++ = 0x52;
  p = ct_push_imm(p, i);
  p = ct_emit32(p, 0xE8, (void *)ft_end);
  *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;
  *p++ = 0x89; *p++ = 0xC1;
  *p++ = 0x5A; *p++ = 0x58;
  *p++ = 0xFF; *p++ = 0xE1;
  if (!VirtualProtect(fn, 5, PAGE_EXECUTE_READWRITE, &old))
    return;
  ct_emit32(fn, 0xE9, code);
  VirtualProtect(fn, 5, old, &old);
  FlushInstructionCache(GetCurrentProcess(), fn, 5);
  InterlockedIncrement(&ft_n);
}

/* Hooks every spec with the given prefix ("lle", "tnl", "exe") in module mod
 * (NULL for "exe").  A renderer loaded again keeps its hooks: an entry that
 * already jumps into our memory is skipped. */
static void ft_install(const char *prefix, HMODULE mod, void (*logf)(const char *fmt, ...)) {
  static char spec[8192];
  char *tok;
  const size_t pl = strlen(prefix);
  if (!ft_spec[0] && !GetEnvironmentVariableA("EE_FUNCTIME", ft_spec, sizeof ft_spec))
    return;
  if (ft_tls == TLS_OUT_OF_INDEXES) {
    ft_tls = TlsAlloc();
    if (ft_tls >= 64) {
      logf("functime: no TLS slot under 64 -- off");
      ft_spec[0] = 0;
      return;
    }
    {
      LARGE_INTEGER q;
      QueryPerformanceCounter(&q);
      ft_q0 = q.QuadPart;
      ft_c0 = (LONGLONG)__builtin_ia32_rdtsc();
    }
  }
  lstrcpynA(spec, ft_spec, sizeof spec);
  for (tok = strtok(spec, ","); tok; tok = strtok(NULL, ",")) {
    unsigned char *fn;
    int k, dup = 0;
    if (strncmp(tok, prefix, pl) || tok[pl] != ':')
      continue;
    fn = mod ? (unsigned char *)GetProcAddress(mod, tok + pl + 1) : (unsigned char *)strtoul(tok + pl + 1, NULL, 16);
    if (!fn) {
      logf("functime: %s not found", tok);
      continue;
    }
    for (k = 0; k < ft_n; k++)
      if (ft_fns[k].fn == fn)
        dup = 1;
    if (!dup)
      ft_hook(fn, tok, logf);
  }
}

static void ft_report(void (*logf)(const char *fmt, ...)) {
  LARGE_INTEGER q, f;
  const LONGLONG c = (LONGLONG)__builtin_ia32_rdtsc();
  double cyc_per_ms, secs;
  int i;
  if (!ft_n)
    return;
  QueryPerformanceCounter(&q);
  QueryPerformanceFrequency(&f);
  secs = (double)(q.QuadPart - ft_q0) / (double)f.QuadPart;
  cyc_per_ms = (double)(c - ft_c0) / (secs * 1000.0);
  ft_q0 = q.QuadPart;
  ft_c0 = c;
  for (i = 0; i < ft_n; i++) {
    const LONG n0 = InterlockedExchange(&ft_fns[i].n[0], 0), n1 = InterlockedExchange(&ft_fns[i].n[1], 0);
    const LONGLONG c0 = InterlockedExchange64(&ft_fns[i].cyc[0], 0), c1 = InterlockedExchange64(&ft_fns[i].cyc[1], 0);
    if (n0 || n1)
      logf("  fn %-44.44s render %8.0f/s %6.1f ms/s (%.2f us)  other %8.0f/s %6.1f ms/s", ft_fns[i].name, n0 / secs,
           c0 / cyc_per_ms / secs, n0 ? c0 / cyc_per_ms * 1000.0 / n0 : 0.0, n1 / secs, c1 / cyc_per_ms / secs);
  }
}
