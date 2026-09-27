/* Saves in huge games (EE_MEMFILE_DOUBLE=0 turns this off).
 *
 * The game builds a save in memory (Low-Level Engine's FSMemoryFile) before
 * writing it out.  Unless created in its doubling mode, such a file grows by a
 * fixed small step: every write past the end allocates a block one step
 * bigger, copies everything into it and frees the old one -- which is always
 * too small for the next step, so the address space fills with holes.
 * Growing a save to 8 MB that way churned through ~870 MB on 27 Sep 2026 (a
 * gigantic map, 1,700 units): the game ran out of its 4 GB, got a null block
 * back and crashed writing to it.  Growing by at least the current size
 * instead makes it a handful of steps; what is written does not change (the
 * file's end is kept apart from its capacity).  Same code in both games. */
typedef void(__attribute__((thiscall)) * memfile_grow_fn)(void *file, unsigned long extra);
static memfile_grow_fn g_memfile_grow;

static void __attribute__((thiscall)) memfile_grow(void *file, unsigned long extra) {
  unsigned long cap = *(unsigned long *)((char *)file + 0x6c);  /* its capacity */
  if (extra < cap)
    extra = cap;
  g_memfile_grow(file, extra);
}

static void memfile_install(HMODULE lle, void (*logf)(const char *fmt, ...)) {
  /* push ebx/esi/edi; mov edi,[esp+0x10]; mov esi,ecx; add edi,[esi+0x6c] */
  static const unsigned char head[12] = {0x53, 0x56, 0x57, 0x8b, 0x7c, 0x24, 0x10, 0x8b, 0xf1, 0x03, 0x7e, 0x6c};
  unsigned char *fn;
  char b[8];
  if (!lle || (GetEnvironmentVariableA("EE_MEMFILE_DOUBLE", b, sizeof b) > 0 && b[0] == '0'))
    return;
  fn = (unsigned char *)GetProcAddress(lle, "?IncreaseFileSize@FSMemoryFile@@IAEXK@Z");
  if (!fn || IsBadReadPtr(fn, 12) || memcmp(fn, head, 12) ||
      !(g_memfile_grow = (memfile_grow_fn)ls_hook(fn, head, 7, (void *)memfile_grow))) {
    logf("saves: FSMemoryFile is not the expected code -- it grows as before");
    return;
  }
  logf("saves: in-memory files grow by doubling (huge-game saves no longer eat the address space)");
}
