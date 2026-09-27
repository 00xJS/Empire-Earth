/* Every unit of a big crowd drawn (EE_BIG_CROWDS=0 turns this off).
 *
 * The game draws the units on screen from a fixed pool of 512 draw slots
 * (0x148 bytes each, at +0x78 of the render lists' global object; the number in
 * use at +0x3f880).  A unit the screen scan finds after the pool is full is not
 * drawn that frame.  The scan goes row by row from the back, so in a crowd of
 * more than 512 the nearest units are the ones left out, and as units step from
 * tile to tile the cut moves and they blink.  (27 Sep 2026: in the owner's big
 * battle every frame drew exactly 512 units.)
 *
 * The pool moves to a bigger one here.  Its address is in seven places in the
 * game's code and its size in two (the scan's check, and the limit it passes
 * on for objects bigger than a tile); every one is checked before any is
 * changed.  The new slots are built with the game's own slot constructor: in
 * Empire Earth right after the game builds its own (this DLL arrives before
 * the game's static constructors run), in Art of Conquest at once (it arrives
 * later).  EE_CROWD_SLOTS=<n> sets the size (default 10000). */

#define CROWD_SLOT 0x148
#define CROWD_OLD 512

struct crowd_site {
  DWORD at;               /* the instruction */
  unsigned char op[2], n; /* its first n opcode bytes */
  unsigned char imm;      /* where its 32-bit immediate starts */
};
struct crowd_game {
  DWORD object;    /* the render lists' global object */
  DWORD slot_ctor; /* a draw slot's constructor (thiscall) */
  DWORD init_jmp;  /* the object's static initialiser, mov ecx,object / jmp <constructor>: the jmp */
  struct crowd_site site[9];
};
static const struct crowd_game g_crowd_games[] = {
    {0x8d2cb8, 0x506442, 0x50009e, /* Empire Earth.exe */
     {{0x5007b1, {0xbe}, 1, 1},       /* mov esi,slots+0x58: the pass over every slot as a game starts */
      {0x5007ca, {0x81, 0xfe}, 2, 2}, /* cmp esi,end+0x58: its end */
      {0x500996, {0x81, 0x3d}, 2, 6}, /* cmp [in use],512: the screen scan's check */
      {0x503c68, {0x68}, 1, 1},       /* push slots: the reset before each scan */
      {0x503ecf, {0xbf}, 1, 1},       /* mov edi,slots: the scan */
      {0x503fcb, {0x05}, 1, 1},       /* add eax,slots+0xfc: the scan */
      {0x504157, {0x68}, 1, 1},       /* push 512: the limit for objects bigger than a tile */
      {0x5043b4, {0x68}, 1, 1},       /* push slots: the reset when a game ends */
      {0x50dc59, {0x68}, 1, 1}}},     /* push slots: a later pass over the slots in use */
    {0x8e9458, 0x5118e9, 0x50ab99, /* EE-AOC.exe, the same code */
     {{0x50b2ac, {0xbe}, 1, 1},
      {0x50b2c5, {0x81, 0xfe}, 2, 2},
      {0x50b491, {0x81, 0x3d}, 2, 6},
      {0x50eba3, {0x68}, 1, 1},
      {0x50ee0a, {0xbf}, 1, 1},
      {0x50ef06, {0x05}, 1, 1},
      {0x50f092, {0x68}, 1, 1},
      {0x50f2ef, {0x68}, 1, 1},
      {0x51854e, {0x68}, 1, 1}}},
};

typedef void *(__attribute__((thiscall)) * crowd_ctor_fn)(void *self);
static crowd_ctor_fn g_crowd_ctor, g_crowd_slot_ctor;
static unsigned char *g_crowd_pool;
static unsigned g_crowd_n;

static void crowd_build(void) {
  unsigned i;
  for (i = 0; i < g_crowd_n; i++)
    g_crowd_slot_ctor(g_crowd_pool + i * CROWD_SLOT);
}

/* The game's static initialiser jumps here in place of the object's
 * constructor (ecx = the object; returns it, as the constructor does). */
static void *__attribute__((thiscall)) crowd_init(void *object) {
  void *r = g_crowd_ctor(object);
  crowd_build();
  return r;
}

/* An immediate of the old pool -> the same place in the new one: its size, an
 * address inside it, or one just past its end. */
static int crowd_move(DWORD old, DWORD pool, DWORD npool, unsigned n, DWORD *out) {
  DWORD end = pool + CROWD_OLD * CROWD_SLOT;
  if (old == CROWD_OLD)
    *out = n;
  else if (old >= pool && old < end)
    *out = npool + (old - pool);
  else if (old >= end && old < end + CROWD_SLOT)
    *out = npool + n * CROWD_SLOT + (old - end);
  else
    return 0;
  return 1;
}

/* early: this DLL arrived with the game, before its static constructors ran
 * (Empire Earth); otherwise they have run (Art of Conquest). */
static void crowd_install(void (*logf)(const char *fmt, ...), int early) {
  static const unsigned char slot_head[10] = {0x55, 0x8b, 0xec, 0x51, 0x56, 0x57, 0x8b, 0xf1, 0xff, 0x15};
  char b[16];
  unsigned gi, i, n = 10000;
  if (GetEnvironmentVariableA("EE_BIG_CROWDS", b, sizeof b) > 0 && b[0] == '0') {
    logf("big crowds: off (EE_BIG_CROWDS=0) -- the game draws at most 512 units");
    return;
  }
  if (GetEnvironmentVariableA("EE_CROWD_SLOTS", b, sizeof b) > 0) {
    for (n = 0, i = 0; b[i] >= '0' && b[i] <= '9' && n < 100000; i++)
      n = n * 10 + (unsigned)(b[i] - '0');
    if (n < CROWD_OLD)
      n = CROWD_OLD;
    if (n > 65536)
      n = 65536;
  }
  for (gi = 0; gi < sizeof g_crowd_games / sizeof g_crowd_games[0]; gi++) {
    const struct crowd_game *g = &g_crowd_games[gi];
    DWORD pool = g->object + 0x78, olds[9], news[9], prot[9], npool, obj, lo = ~0u, hi = 0, old;
    unsigned char *j = (unsigned char *)(ULONG_PTR)g->init_jmp, *slot = (unsigned char *)(ULONG_PTR)g->slot_ctor;
    int rel, bad = -1, now;
    if (IsBadReadPtr(j - 5, 10) || IsBadReadPtr(slot, 10) || memcmp(slot, slot_head, 10) || j[-5] != 0xb9 ||
        j[0] != 0xe9 || IsBadReadPtr((void *)(ULONG_PTR)pool, CROWD_OLD * CROWD_SLOT))
      continue;
    memcpy(&obj, j - 4, 4);
    if (obj != g->object)
      continue;
    for (i = 0; i < 9 && bad < 0; i++) {
      const struct crowd_site *s = &g->site[i];
      unsigned char *c = (unsigned char *)(ULONG_PTR)s->at;
      if (IsBadReadPtr(c, s->imm + 4) || memcmp(c, s->op, s->n) ||
          !crowd_move(*(DWORD *)(c + s->imm), pool, pool, CROWD_OLD, &olds[i]))
        bad = (int)i;
      else if (s->imm == 6 && *(DWORD *)(c + 2) != g->object + 0x3f880) /* the check must be of the in-use count */
        bad = (int)i;
      else {
        olds[i] = *(DWORD *)(c + s->imm);
        if (s->at < lo)
          lo = s->at;
        if (s->at + s->imm + 4 > hi)
          hi = s->at + s->imm + 4;
      }
    }
    if (bad >= 0) {
      logf("big crowds: the draw-slot code at %08lx is not what was analysed -- the game draws at most 512 units",
           (unsigned long)g->site[bad].at);
      return;
    }
    g_crowd_pool = (unsigned char *)VirtualAlloc(NULL, n * CROWD_SLOT, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_crowd_pool) {
      logf("big crowds: no memory for %u draw slots -- the game draws at most 512 units", n);
      return;
    }
    g_crowd_n = n;
    g_crowd_slot_ctor = (crowd_ctor_fn)(ULONG_PTR)g->slot_ctor;
    npool = (DWORD)(ULONG_PTR)g_crowd_pool;
    for (i = 0; i < 9; i++)
      crowd_move(olds[i], pool, npool, n, &news[i]);
    /* The game's slot 0 has its scale (+0x40) at 1.0 once built: then its
     * constructors have run, whatever the load order said. */
    now = !early || *(DWORD *)(ULONG_PTR)(pool + 0x40) == 0x3f800000;
    if (now)
      crowd_build(); /* before any code can reach them */
    else {
      memcpy(&rel, j + 1, 4);
      g_crowd_ctor = (crowd_ctor_fn)(ULONG_PTR)(g->init_jmp + 5 + rel);
      rel = (int)((DWORD)(ULONG_PTR)crowd_init - (g->init_jmp + 5));
      if (!VirtualProtect(j + 1, 4, PAGE_EXECUTE_READWRITE, &old)) {
        logf("big crowds: could not hook the static initialiser -- the game draws at most 512 units");
        return;
      }
      memcpy(j + 1, &rel, 4);
      VirtualProtect(j + 1, 4, old, &old);
      FlushInstructionCache(GetCurrentProcess(), j, 5);
    }
    /* all nine or none: every site made writable before any is written, and
     * each given back its own protection after */
    for (i = 0; i < 9; i++)
      if (!VirtualProtect((unsigned char *)(ULONG_PTR)g->site[i].at + g->site[i].imm, 4, PAGE_EXECUTE_READWRITE,
                          &prot[i]))
        break;
    if (i < 9) {
      while (i--)
        VirtualProtect((unsigned char *)(ULONG_PTR)g->site[i].at + g->site[i].imm, 4, prot[i], &old);
      logf("big crowds: could not write the draw-slot code -- the game draws at most 512 units");
      return;
    }
    for (i = 0; i < 9; i++)
      memcpy((unsigned char *)(ULONG_PTR)g->site[i].at + g->site[i].imm, &news[i], 4);
    for (i = 0; i < 9; i++)
      VirtualProtect((unsigned char *)(ULONG_PTR)g->site[i].at + g->site[i].imm, 4, prot[i], &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)(ULONG_PTR)lo, hi - lo);
    logf("big crowds: %u draw slots for units on screen (was 512)%s", n, now ? "" : ", built after the game's own");
    return;
  }
  logf("big crowds: not the analysed exe -- the game draws at most 512 units");
}
