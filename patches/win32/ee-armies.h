/* Bigger armies (EE_BIG_ARMIES=0 turns this off; do that for LAN games).
 *
 * Game Unit Limit: its range is record 26 of dbworld.dat (50 to 1200), which
 * the Scenario Editor's slider shows and which the game-settings copy enforces
 * (a bigger value is reset to 50 there).  Its maximum is raised to 10000 as the
 * table loads.  The random-map setup list has its own loop to 1200 (`cmp
 * esi,1200`), raised to match; the handler maps entry and value linearly, so
 * the step stays 50.  The engine keeps units in growable lists; the cost of a
 * big number is speed, since every simulation step walks every unit.
 *
 * Selection: a drag-select is one game message whose unit list is packed into
 * a 255-byte field, and the packer stops at 63 units (`mov [ebp-8],0x3f`).
 * Up to 63 units still go through the game's own packer, bit for bit.  A
 * bigger selection is kept whole in a table in this process and the message
 * carries its ticket instead -- six bytes that the game's own unpacker reads
 * as empty groups -- and the unpacker, where the message is built and where it
 * is played, swaps the ticket back for the full list.  Another machine could
 * not redeem a ticket, hence the LAN note above. */

struct armies_sites {
  DWORD world;      /* the dbworld table object (records at +8, 0x1c bytes each) */
  DWORD world_load; /* its Load(FSFileSpec&), thiscall */
  DWORD combo_cmp;  /* cmp esi,0x4b0 in the setup screen's unit limit list */
  DWORD pack;       /* the selection packer: cdecl (units vector*, player, out*) -> length */
  DWORD unpack;     /* its unpacker: cdecl (data*, length, units vector*) */
  DWORD vec_clear;  /* the units vector's clear and push_back (thiscall) */
  DWORD vec_push;
};
static const struct armies_sites g_armies_sites[] = {
    {0x917bb0, 0x4d6c57, 0x67b000, 0x4bd15a, 0x4bd2c4, 0x526aff, 0x40fecc}, /* Empire Earth.exe */
    {0x92fca0, 0x4dcb79, 0x694643, 0x4c5e67, 0x4c5fd1, 0x5b5772, 0x4111b8}, /* EE-AOC.exe */
};

typedef unsigned char(__cdecl *armies_pack_fn)(void *units, unsigned player, unsigned char *out);
typedef void(__cdecl *armies_unpack_fn)(const unsigned char *data, unsigned len, void *units);
typedef void(__attribute__((thiscall)) * armies_clear_fn)(void *units);
typedef void(__attribute__((thiscall)) * armies_push_fn)(void *units, const unsigned *id);

#define ARMIES_SLOTS 32
#define ARMIES_MAX 10000
static armies_pack_fn g_armies_pack;
static armies_unpack_fn g_armies_unpack;
static armies_clear_fn g_armies_clear;
static armies_push_fn g_armies_push;
static unsigned g_armies_ids[ARMIES_SLOTS][ARMIES_MAX];
static unsigned g_armies_n[ARMIES_SLOTS], g_armies_tag[ARMIES_SLOTS];
static volatile LONG g_armies_seq;
static void (*g_armies_log)(const char *fmt, ...);

/* record 26 of the world table: +0x0c its index, +0x10 the least unit limit,
 * +0x14 the most */
typedef long(__attribute__((thiscall)) * armies_load_fn)(void *table, void *spec);
static armies_load_fn g_armies_load;
static void *g_armies_world;

static void armies_world_patch(void) {
  unsigned char *rec = g_armies_world ? *(unsigned char **)((char *)g_armies_world + 8) : NULL;
  int *r;
  if (!rec || IsBadWritePtr(rec + 26 * 0x1c, 0x1c))
    return;
  r = (int *)(rec + 26 * 0x1c);
  if (r[3] == 26 && r[4] == 50 && r[5] == 1200) {
    r[5] = 10000;
    if (g_armies_log)
      g_armies_log("big armies: dbworld's Game Unit Limit range is now 50-10000");
  }
}

static long __attribute__((thiscall)) armies_load(void *table, void *spec) {
  long r = g_armies_load(table, spec);
  if (table == g_armies_world)
    armies_world_patch();
  return r;
}

static unsigned char __cdecl armies_pack(void *units, unsigned player, unsigned char *out) {
  unsigned *b = *(unsigned **)((char *)units + 4), *e = *(unsigned **)((char *)units + 8);
  unsigned n = b ? (unsigned)(e - b) : 0, t, k, i;
  if (n <= 63)
    return g_armies_pack(units, player, out);
  t = (unsigned)InterlockedIncrement(&g_armies_seq) & 0xffffff;
  k = t % ARMIES_SLOTS;
  if (n > ARMIES_MAX)
    n = ARMIES_MAX;
  memcpy(g_armies_ids[k], b, n * sizeof(unsigned));
  g_armies_n[k] = n;
  g_armies_tag[k] = t;
  if (g_armies_log && t <= 20)
    g_armies_log("big armies: a selection of %u units (ticket %u)", n, t);
  out[0] = (unsigned char)player;
  out[1] = 0xfe; /* group header "mode 3, 62 units": the game's unpacker skips it */
  for (i = 0; i < 4; i++)
    out[2 + i] = (unsigned char)(0xc0 | ((t >> (6 * i)) & 0x3f));
  return 6;
}

static void __cdecl armies_unpack(const unsigned char *data, unsigned len, void *units) {
  unsigned t = 0, k, i;
  if ((len & 0xff) == 6 && data[1] == 0xfe && (data[2] & data[3] & data[4] & data[5] & 0xc0) == 0xc0) {
    for (i = 0; i < 4; i++)
      t |= (unsigned)(data[2 + i] & 0x3f) << (6 * i);
    k = t % ARMIES_SLOTS;
    if (g_armies_tag[k] == t && g_armies_n[k]) {
      g_armies_clear(units);
      for (i = 0; i < g_armies_n[k]; i++)
        g_armies_push(units, &g_armies_ids[k][i]);
      return;
    }
  }
  g_armies_unpack(data, len, units);
}

static void armies_install(void (*logf)(const char *fmt, ...)) {
  static const unsigned char load_head[14] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x30, 0x83,
                                              0x65, 0xfc, 0x00, 0x83, 0x65, 0xec, 0x00};
  static const unsigned char cmp[8] = {0x81, 0xfe, 0xb0, 0x04, 0x00, 0x00, 0x7e, 0xbd};
  static const unsigned char pack_head[6] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x28};
  static const unsigned char pack_63[7] = {0xc7, 0x45, 0xf8, 0x3f, 0x00, 0x00, 0x00}; /* at +0x31 */
  static const unsigned char unpack_head[5] = {0x55, 0x8b, 0xec, 0x51, 0x51};
  static const unsigned char clear_head[6] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x0c};
  static const unsigned char push_head[6] = {0x55, 0x8b, 0xec, 0x51, 0x56, 0xff};
  char b[8];
  unsigned i;
  if (GetEnvironmentVariableA("EE_BIG_ARMIES", b, sizeof b) > 0 && b[0] == '0')
    return;
  for (i = 0; i < sizeof g_armies_sites / sizeof g_armies_sites[0]; i++) {
    const struct armies_sites *s = &g_armies_sites[i];
    unsigned char *c = (unsigned char *)(ULONG_PTR)s->combo_cmp, *p = (unsigned char *)(ULONG_PTR)s->pack,
                  *u = (unsigned char *)(ULONG_PTR)s->unpack;
    int rel;
    DWORD old;
    unsigned char *w = (unsigned char *)(ULONG_PTR)s->world_load;
    if (IsBadReadPtr(w, 14) || memcmp(w, load_head, 14) || IsBadReadPtr((void *)(ULONG_PTR)s->world, 12) ||
        IsBadReadPtr(c, 8) || memcmp(c, cmp, 8) || IsBadReadPtr(p, 0x38) || memcmp(p, pack_head, 6) ||
        memcmp(p + 0x31, pack_63, 7) || IsBadReadPtr(u, 0x87) || memcmp(u, unpack_head, 5) ||
        IsBadReadPtr((void *)(ULONG_PTR)s->vec_clear, 6) || IsBadReadPtr((void *)(ULONG_PTR)s->vec_push, 6) ||
        memcmp((void *)(ULONG_PTR)s->vec_clear, clear_head, 6) || memcmp((void *)(ULONG_PTR)s->vec_push, push_head, 6))
      continue;
    /* the unpacker's own calls must be the helpers named above */
    memcpy(&rel, u + 0x13, 4);
    if (u[0x12] != 0xe8 || (DWORD)(ULONG_PTR)(u + 0x17 + rel) != s->vec_clear)
      continue;
    memcpy(&rel, u + 0x83, 4);
    if (u[0x82] != 0xe8 || (DWORD)(ULONG_PTR)(u + 0x87 + rel) != s->vec_push)
      continue;
    g_armies_log = logf;
    g_armies_world = (void *)(ULONG_PTR)s->world;
    armies_world_patch(); /* already loaded (AoC can arrive late) */
    if (!(g_armies_load = (armies_load_fn)ls_hook(w, load_head, 6, (void *)armies_load)))
      logf("big armies: the world table loader would not hook -- the editor stays at 1200");
    g_armies_clear = (armies_clear_fn)(ULONG_PTR)s->vec_clear;
    g_armies_push = (armies_push_fn)(ULONG_PTR)s->vec_push;
    if (!(g_armies_unpack = (armies_unpack_fn)ls_hook(u, unpack_head, 5, (void *)armies_unpack)))
      continue;
    if (!(g_armies_pack = (armies_pack_fn)ls_hook(p, pack_head, 6, (void *)armies_pack)))
      logf("big armies: the selection packer would not hook -- selections stay at 63");
    if (VirtualProtect(c + 2, 4, PAGE_EXECUTE_READWRITE, &old)) {
      int top = 10000;
      memcpy(c + 2, &top, 4);
      VirtualProtect(c + 2, 4, old, &old);
      FlushInstructionCache(GetCurrentProcess(), c, 8);
    }
    logf("big armies: Game Unit Limit up to 10000, drag-select takes the whole box%s",
         g_armies_pack ? "" : " (not hooked)");
    return;
  }
  logf("big armies: not the analysed exe -- unit limit stays at 1200, selections at 63");
}
