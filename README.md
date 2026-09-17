# Empire Earth on macOS

Run **your own already-purchased GOG copy** of Empire Earth Gold Edition (2001) on
Apple Silicon, using only free tooling.

This is not a port and not a rewrite. It is a launcher plus a set of compatibility
shims around [Wine](https://wiki.winehq.org/MacOS). **No Empire Earth binaries or
assets are in this repository**, and none ever will be — you supply the game.

You do **not** need CrossOver, Parallels, or any other paid compatibility layer.
Wine, Homebrew, winetricks and Rosetta 2 are free.

## Status

The game **reaches its main menu and renders continuously** on an M2 Pro running
macOS 26, at 1024×768 in a window, at roughly 175 FPS.

Actual gameplay — starting a skirmish or a campaign — is **not yet verified**.
See [Known issues](#known-issues).

## Requirements

- Apple Silicon Mac (macOS 14 or later)
- Your GOG Empire Earth Gold folder, or the GOG Windows `setup_*.exe`
- [Homebrew](https://brew.sh) (used only to install free `winetricks` and `mingw-w64`)
- Rosetta 2 (the launcher installs it if needed)

Retail CD / SafeDisc copies are not supported. Use the DRM-free GOG files.

## First run

```bash
chmod +x scripts/*.sh
./scripts/install-wine.sh
./scripts/setup-prefix.sh
./scripts/set-game.sh "/path/to/Empire Earth Gold Edition"
```

`setup-prefix.sh` installs DirectMusic, which Empire Earth needs or it crashes at
startup. If you have the GOG installer rather than an installed folder, point
`set-game.sh` at the `setup_*.exe` instead and it will install into the bottle.

## Play

```bash
./scripts/launch.sh          # base game
./scripts/launch.sh aoc      # Art of Conquest
```

No environment variables are needed — the working configuration is the default.
There is also a SwiftUI launcher:

```bash
./scripts/build-app.sh
open "dist/Empire Earth.app"
```

### If you get stuck on screen

The game runs inside a Wine virtual desktop, so it should behave like a normal
window. If it ever traps your pointer:

| | |
|---|---|
| `Ctrl`+`Alt`+`Q` | release the mouse and let go of the window |
| `Ctrl`+`Alt`+`X` | quit the game |
| `./scripts/stop.sh` | panic button — stops the game, the desktop and wineserver |

## How it actually works

Empire Earth is a 32-bit Direct3D 7 title. The chain is:

```
Empire Earth.exe  (32-bit PE, under Rosetta 2)
  → ddraw.dll         our instrumenting DirectDraw proxy  (patches/win32/ee-ddraw.c)
  → D7VK              DXVK-Sarek's Direct3D 7 → Direct3D 9 layer
  → d3d9.dll          DXVK-Sarek, Direct3D 9 → Vulkan
  → libMoltenVK       Vulkan → Metal, via our shim        (patches/vulkan/ee-vkfix.c)
```

### The bug that blocked this for months

The game asks for `SetCooperativeLevel(DDSCL_FULLSCREEN | DDSCL_EXCLUSIVE)`.
Wine's macOS driver then **minimizes** that window whenever the application is
deactivated. DXVK's D3D9 swapchain reports device-lost for an iconic window, D7VK
translates that to `DDERR_SURFACELOST` on the game's very first `Flip`, and Empire
Earth responds by destroying the device and re-enumerating adapters — forever.

That loop is the "goes fullscreen, then black" symptom. It is also why there was
no sound: the game never got far enough to initialise Miles.

The proxy caught it in one line:

```
Surface::Flip  hr=0x887601c2 DDERR_SURFACELOST
window[flip-failed]  rect=-32000,-32000,-31840,-31969  iconic=1  fg=(OTHER)
```

`-32000,-32000` is Windows' canonical position for a minimized window.

Two things fix it, and both are on by default:

1. **A Wine virtual desktop**, so the game's window is never a macOS fullscreen
   window and nothing minimizes it.
2. **A watchdog thread** in the ddraw proxy that un-minimizes and re-foregrounds
   the device window during startup, then hands the window back to you as soon as
   the first frame renders.

### Why the virtual desktop is 1024×801

The game runs at 1024×768, but its window takes a constant +33px vertical offset
when it changes mode — which clipped the bottom of the main menu, including
**Exit Game**. The desktop is 33 rows taller to absorb that.

## Known issues

- **~36% of launches die in about 3 seconds** inside `wow64cpu.dll+0x123d`, Wine's
  own 32↔64-bit transition dispatcher, under Rosetta. It is environmental, not
  this project's code, and it is invisible to everything above it. `launch.sh`
  detects the signature and retries automatically (`EE_LAUNCH_ATTEMPTS`, default 3).
- **The depth buffer clear fails on every frame** — `Device7::Clear flags=0x2`
  returns `0x88760816`. Harmless for the 2D menu, but it will matter in a
  skirmish, where terrain and units need depth testing.
- **Gameplay is unverified.** The menu renders; nobody has played a match yet.
- **Music is off by default.** It crashed under Wine in earlier testing. Sound
  effects should work now that the game gets far enough to initialise Miles, but
  this has not been confirmed.
- Exclusive fullscreen is unreachable on this stack — MoltenVK does not implement
  `VK_EXT_full_screen_exclusive`. A window is the honest end state. Chasing
  `DDSCL_EXCLUSIVE` is what produced the bug above in the first place.
- Multiplayer patches such as NeoEE are optional and not bundled here.

## Graphics stacks

`d7vk` is the default and the only stack that reaches the menu. The others are
kept because they were useful for bisecting.

| Stack | What it is |
|---|---|
| `d7vk` | **D7VK / DXVK-Sarek — native D3D7 → D3D9 → Vulkan. The one that works.** |
| `dgvoodoo` | dgVoodoo 2.79.3 + DXVK D3D11 (2.82+ crashes inside DDraw on Wine) |
| `gog-d3d9` | GOG's D3D7 → D3D9 wrapper + DXVK D3D9 |
| `dgvoodoo-wined3d` | dgVoodoo + Wine's builtin D3D11 |
| `dxmt` | dgVoodoo + DXMT 0.80 on Wine Devel |
| `d3dmetal` | Apple D3DMetal/GPTK — 64-bit only, so a 32-bit EXE cannot use it |

```bash
./scripts/set-options.sh --graphics d7vk --virtual-desktop on --virtual-desktop-size 1024x801
```

Wrapper DLLs are fetched into Application Support by
`scripts/install-compat-patches.sh` and `scripts/install-d7vk.sh`. They are **not**
committed here.

## Useful switches

| Variable | Effect |
|---|---|
| `EE_MENU_TIMEOUT` | seconds to wait for the menu (default 240) |
| `EE_LAUNCH_ATTEMPTS` | retries for the wow64 early crash (default 3) |
| `EE_DDRAW_VERBOSE=1` | full per-call DirectDraw logging (very large) |
| `EE_DDRAW_KEEP_FOREGROUND=0` | disable the window watchdog |
| `EE_VKFIX_SIZE=WxH` | override the MoltenVK shim's fallback extent |
| `EE_VKFIX_FORCE_EXTENT=1` | force every Vulkan surface to that extent |
| `DXVK_LOG_LEVEL=info` | restore DXVK's verbose logging |

## Where to get the game files

GOG Galaxy on Mac will not install this Windows-only game. In a browser, open
[your GOG account](https://www.gog.com/en/account), find **Empire Earth Gold
Edition**, and download the **Windows offline backup installer**
(`setup_empire_earth_*.exe`). Then point `set-game.sh` at that file.

If you already installed the game on a Windows PC, copy the folder containing
`Empire Earth.exe` to this Mac and point `set-game.sh` at that folder instead.

Logs live in `~/Library/Application Support/EmpireEarthMac/logs`.
