/* Mac keys for the game's hotkeys (EE_MAC_KEYS=0 turns this off).
 *
 * A Mac's delete key is Backspace, and MacBooks have no numeric keypad, so
 * three in-game hotkeys can't be pressed as bound: Del (kill the first
 * selected unit; Shift+Del kills them all) and the keypad's + and - (game
 * speed).  None of Backspace, = or - is bound in a match (the exe registers
 * all 61 in-game hotkeys in one function, EE 0x64cc60-0x64dbad), so each
 * hotkey bound to one of those keys also answers to its Mac key:
 * UIHotKey::FireEvents, when the hotkey did not fire, runs once more with the
 * alias key's entity in place of its own.  Modifiers are checked as usual
 * (Shift+delete = Shift+Del), and a hotkey the game has disabled stays
 * disabled.  Text boxes read keys themselves, not through hotkeys, so typing
 * is unaffected.  Same code in both games' engines. */

/* UIHotKey: +0x08 the key's ISEntity; ISKeyboard::GetEntity(index) is vtable
 * slot 0 and indexes by DirectInput scan code */
typedef unsigned char(__attribute__((thiscall)) * keys_fire_fn)(void *hotkey);
typedef long(__attribute__((thiscall)) * keys_entity_fn)(void *keyboard, unsigned long index, void **entity);

static keys_fire_fn g_keys_fire;  /* the original FireEvents (trampoline) */
static void **g_keys_kbd;         /* &gISKeyboard */
static struct {
  unsigned char key, alias;
  void *ekey, *ealias;
} g_keys_alias[] = {
    {0xd3, 0x0e}, /* Del <- delete (Backspace) */
    {0x4e, 0x0d}, /* keypad + <- = */
    {0x4a, 0x0c}, /* keypad - <- - */
};

static void *keys_entity(unsigned idx) {
  void *kbd = g_keys_kbd ? *g_keys_kbd : NULL, *e = NULL;
  if (!kbd || (*(keys_entity_fn **)kbd)[0](kbd, idx, &e) != 0)
    return NULL;
  return e;
}

static unsigned char __attribute__((thiscall)) keys_fire(void *hotkey) {
  unsigned char fired = g_keys_fire(hotkey);
  void **slot = (void **)((char *)hotkey + 8), *key = *slot;
  unsigned i;
  if (fired || !key)
    return fired;
  for (i = 0; i < sizeof g_keys_alias / sizeof g_keys_alias[0]; i++) {
    if (!g_keys_alias[i].ekey) { /* the keyboard exists once the game has started */
      g_keys_alias[i].ekey = keys_entity(g_keys_alias[i].key);
      g_keys_alias[i].ealias = keys_entity(g_keys_alias[i].alias);
    }
    if (key == g_keys_alias[i].ekey && g_keys_alias[i].ealias) {
      *slot = g_keys_alias[i].ealias;
      fired = g_keys_fire(hotkey);
      *slot = key;
      break;
    }
  }
  return fired;
}

static void keys_install(HMODULE lle, void (*logf)(const char *fmt, ...)) {
  static const unsigned char head[6] = {0x53, 0x56, 0x8b, 0xf1, 0x33, 0xdb}; /* push ebx/esi, mov esi,ecx, xor ebx,ebx */
  char b[8];
  unsigned char *fire;
  if (!lle || (GetEnvironmentVariableA("EE_MAC_KEYS", b, sizeof b) > 0 && b[0] == '0'))
    return;
  fire = (unsigned char *)GetProcAddress(lle, "?FireEvents@UIHotKey@@UAE_NXZ");
  g_keys_kbd = (void **)GetProcAddress(lle, "?gISKeyboard@@3PAVISKeyboard@@A");
  if (!g_keys_kbd || !(g_keys_fire = (keys_fire_fn)ls_hook(fire, head, 6, (void *)keys_fire))) {
    logf("mac keys: UIHotKey::FireEvents is not the expected code -- no aliases");
    return;
  }
  logf("mac keys: delete = Del (kill units), = and - = keypad + and - (game speed)");
}
