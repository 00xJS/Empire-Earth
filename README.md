# Empire Earth on macOS

Run **your own already-purchased GOG copy** of Empire Earth Gold Edition (2001) on
Apple Silicon, using only free tooling.

This is not a port and not a rewrite. It is a launcher plus a set of compatibility
shims around [Wine](https://wiki.winehq.org/MacOS). **No Empire Earth binaries or
assets are in this repository**, and none ever will be — you supply the game.

You do **not** need CrossOver, Parallels, or any other paid compatibility layer.
Wine, Homebrew, winetricks and Rosetta 2 are free.

## Status

**Playable.** On an M2 Pro running macOS 27 (September 2026), Empire Earth
starts, reaches the main menu, and plays random-map games and the tutorial
campaign **full screen** at the display's own resolution (1512×982 on a 14"
MacBook Pro), with sound effects and music. Early in a match it runs at 105–120
FPS. A 26-minute random map ran without a single freeze. The Art of Conquest
expansion reaches its menu and plays random maps the same way. The menu, which
the game fixes at 1024×768, is scaled to the full screen height with black bars
at the sides, and the cursor moves over it cleanly.

A Gigantic random map loads in about 16 seconds, and the load carries on while
you are in another app.

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

### Switching away, and getting out

The game covers the whole screen and hides the menu bar and Dock while it is in
front. `Cmd`+`Tab` away and other apps come to the front as usual (the game pauses
itself in the background); `Cmd`+`Tab` back to **wine** to carry on.

| | |
|---|---|
| `Control`+`Option`+`Q` | release the mouse and minimize the game |
| `Control`+`Option`+`X` | quit the game |
| `./scripts/stop.sh` | panic button — stops the game, the desktop and wineserver |

(`Option` is the Mac's `Alt` key. A quick tap is enough.)

## How it actually works

Empire Earth is a 32-bit Direct3D 7 title. The chain is:

```
Empire Earth.exe  (32-bit PE, under Rosetta 2)
  → ddraw.dll         our instrumenting DirectDraw proxy  (patches/win32/ee-ddraw.c)
  → D7VK              DXVK-Sarek's Direct3D 7 → Direct3D 9 layer
  → d3d9.dll          DXVK-Sarek, Direct3D 9 → Vulkan
  → libMoltenVK       Vulkan → Metal, via our shim        (patches/vulkan/ee-vkfix.c)
```

### Freezes in long matches

Matches used to drop to 0 FPS for 10–50 seconds at a time after a few minutes,
with the game taking around 80,000 page faults a second. DXVK's memory for
reading the screen back is ordinary 32-bit process memory handed to MoltenVK,
and Wine maps all 32-bit memory readable, writable **and executable**.
Rosetta's handling of writable, executable pages is what stalled. The MoltenVK
shim now marks that memory read-write only as it is imported
(`EE_VKFIX_NOEXEC=0` turns this off).

### The cursor, and why the ddraw proxy flips pages itself

Empire Earth draws its own cursor and expects real two-page flipping: after a
`Flip`, the back buffer holds the page that was on screen before. The game wipes
the old cursor off that page with the background it saved underneath it. A
second thread moves the cursor directly on the visible page between frames. D7VK
presents through Direct3D 9 and never swaps DirectDraw's pages, so the saved
backgrounds picked up old cursors, which left trails across the menus.

The proxy now swaps the two pages itself on every flip. It keeps its own copy of
the visible page, because D7VK overwrites its own copy mid-frame. Match frames
repaint the whole screen, so there it only copies the finished frame.

Two related fixes:

- **DXVK shader compilation.** The launcher used to set it to `async`, which
  *skips* a draw until its shader is compiled. The first frame of every menu
  screen therefore came out black, and the cursor saved that black: the black
  squares. It now uses DXVK-Sarek's default, `dyasync`, which draws with a close
  already-compiled shader instead.
- **Blits on the CPU.** The game's small cursor blits used to run on the GPU in
  Wine's wined3d (Apple OpenGL). That forced the whole frame to be read back out
  of OpenGL every time the game locked the screen to draw its text, which it does
  every frame. The proxy now does those blits on the CPU. That removed about 30%
  of each match frame and took an early match from about 75 FPS to over 100.
- **Animation Smoothing off.** The game blends every unit's animation between
  keyframes on the CPU, in old x87 code that Rosetta runs slowly; with 150+
  units on screen that was half of every frame. The launcher turns it off (the
  game only has it as a registry setting), so units step between poses.

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

The root cause is Wine's own "focus window" hook, which marks the DirectDraw
device lost on every focus change. The ddraw proxy now declines that hook, vetoes
the minimize calls (from the game and from Wine) that follow a focus change, and
restores surfaces if a `Flip` still reports them lost. A watchdog thread also
un-minimizes the window during startup and hands it back once the first frame
renders.

### Full screen

The launcher reads the display size and sets the game's `Game Window
Width`/`Height` to match, so matches render at the display's native resolution.
On a MacBook with a camera notch they use the Mac's own below-notch mode instead
(1512×945 on a 14" MacBook Pro): Wine would centre that picture, leaving the
middle of the game's top bar under the notch, so the MoltenVK shim pins it to
the bottom edge and the notch covers only black. `--game-resolution 1512x982`
brings back the full height.
Wine emulates the game's display-mode changes (its `EmulateModeset` setting), so
when the game switches to its fixed 1024×768 menu mode, Wine scales that window
to the full screen height, centred, instead of changing the Mac's resolution.
Two small fixes make that work with Vulkan: the ddraw proxy reports the game
window's own size as the Vulkan surface size, and the MoltenVK shim lets Core
Animation do the scaling. Without them DXVK rebuilt its swapchain on every frame.
The shim also adds a black backdrop and hides the menu bar and Dock while the
game is in front.

`EE_EMULATE_MODESET=0` goes back to the older setup: a Wine virtual desktop the
size of the display, where the menu sits 1:1 in the top-left corner.

Two things make switching apps work: the proxy removes the "always on top" style
that Wine's DirectDraw gives an exclusive window (Wine 11 otherwise keeps such a
window above every app even in the background), and the shim hands the game the
keyboard focus when you switch back, which is what ends its pause.

`./scripts/set-options.sh --fullscreen off` returns to the old windowed setup;
`--game-resolution WxH` picks a different match resolution.

### Switching apps without freezing the game

Switching away could freeze a match for good: no picture, and no way to quit
it except `stop.sh`. When the game loses focus, its window thread waits for its
cursor-drawing thread, and that thread first takes the game's UI lock. The game
reads DirectInput while holding that lock, and Wine's DirectInput handles
pending window messages inside those reads, so a switch that landed there ran
the "focus lost" handler inside the lock: each thread waited on the other. The
proxy now delivers the game's `WM_ACTIVATEAPP` for losing focus from its own
message loop instead, where the lock is never held; regaining focus is
delivered as before.

### Slow match loads (fixed)

On 22 Sep 2026 one random map took about seven minutes to load, which was put
down to a Mac short of memory (4.7 GB of swap in use). It was two bugs since
fixed: the Vulkan page-fault storm above, and the focus-loss deadlock. Retested
on a 16 GB Mac on 23 Sep 2026, a Gigantic random map loaded in 16 s with nothing
else running, 16 s with 8 GB of other memory held (6.6 GB of swap in use), and
16 s with that 8 GB constantly in use — only the frame rate dropped then,
from about 150 to 107 FPS. With the game in the background it kept loading.

### Music

The soundtrack is adaptive DirectMusic (`Data/Music/*.sgt`, `.sty`, `.dls`),
played by the native DirectMusic that `setup-prefix.sh` installs. It used to be
forced off on every launch, by `apply-launch-patches.sh` and by
`patches/wine/empire-earth.reg`, on an old report that it crashed under Wine;
nothing here ever showed that. Music now belongs to the game's own Options →
Music Quality (Off, Low, High) and the launcher's music checkbox, which sets
the same value. It plays in matches; the menus have none.

## Known issues

- The launcher retries a start that dies or stalls before its first frame
  (`EE_LAUNCH_ATTEMPTS`, default 6). The main cause of such deaths — a Rosetta
  race in Wine's 32↔64-bit thunks (`wow64cpu.dll+0x123d`/`+0x1139`) — is fixed by
  `patches/wine/patch-wow64cpu.py`, which the installer applies. About one start
  in ten still stalls on the opening banner (the game's own thread stuck in
  Wine's `DestroyWindow` on a 16×16 test window); the retry covers it.
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
./scripts/set-options.sh --graphics d7vk --virtual-desktop on --virtual-desktop-size 1440x933
```

Wrapper DLLs are fetched into Application Support by
`scripts/install-compat-patches.sh` and `scripts/install-d7vk.sh`. They are **not**
committed here.

## Useful switches

| Variable | Effect |
|---|---|
| `EE_MENU_TIMEOUT` | seconds to wait for the menu (default 240) |
| `EE_LAUNCH_ATTEMPTS` | retries for a start that dies or stalls before its first frame (default 6) |
| `EE_DDRAW_VERBOSE=1` | full per-call DirectDraw logging (very large) |
| `EE_DDRAW_KEEP_FOREGROUND=0` | disable the window watchdog |
| `EE_DDRAW_FOCUS_HOOK=1` | let Wine's focus hook run again (focus changes lose surfaces) |
| `EE_DDRAW_ALLOW_MINIMIZE=1` | let the game and Wine minimize the window on focus loss |
| `EE_DDRAW_KEEP_TOPMOST=1` | keep Wine's "always on top" style on the game window |
| `EE_DDRAW_AUTO_RESTORE=0` | do not restore surfaces when a `Flip` reports them lost |
| `EE_DDRAW_WNDTRACE=0` | stop logging the game window's focus and size messages |
| `EE_DDRAW_SYNC_ACTIVATEAPP=1` | deliver "focus lost" to the game at once again (can freeze it when you switch apps) |
| `EE_EMULATE_MODESET=0` | use a virtual desktop instead of Wine-emulated display modes (menu 1:1, top-left) |
| `EE_VKFIX_NOEXEC=0` | leave Vulkan's imported memory executable (brings back the long freezes) |
| `EE_ANIMATION_SMOOTHING=1` | turn the game's Animation Smoothing back on (off by default: with 150+ units on screen it cost half of every frame under Rosetta) |
| `EE_DDRAW_PAGES=0` | stop the proxy's page flipping (brings back the menu cursor trails) |
| `EE_DDRAW_SOFTBLT=0` | run the game's flip-chain blits on the GPU again (slower) |
| `EE_WINED3D_CSMT=1` | turn wined3d's command-stream thread back on |
| `DXVK_SHADER_COMPILATION_METHOD` | `dyasync` (default), `none` (compile on the spot) or `async` (skips draws; causes black first frames) |
| `EE_DDRAW_PAGEDUMP=1` | debug: save the first flips of each screen as BMPs in the game folder |
| `EE_WOW64CPU_PATCH=0` | restore Wine's original `wow64cpu.dll` |
| `EE_VKFIX_SIZE=WxH` | override the MoltenVK shim's fallback extent |
| `EE_VKFIX_FORCE_EXTENT=1` | force every Vulkan surface to that extent |
| `EE_DDRAW_STRETCH=1` | grow the game window to fill the desktop (scales the picture up) |
| `EE_VKFIX_FLOAT=1` | keep the game window above all other applications |
| `EE_VEH=1` | install the exception handler used to identify Wine crashes |
| `DXVK_LOG_LEVEL=info` | restore DXVK's verbose logging |

## Where to get the game files

GOG Galaxy on Mac will not install this Windows-only game. In a browser, open
[your GOG account](https://www.gog.com/en/account), find **Empire Earth Gold
Edition**, and download the **Windows offline backup installer**
(`setup_empire_earth_*.exe`). Then point `set-game.sh` at that file.

If you already installed the game on a Windows PC, copy the folder containing
`Empire Earth.exe` to this Mac and point `set-game.sh` at that folder instead.

## Seeing the game window

The game is launched as a bare `wine` process, so macOS does not treat it as a
registered application — some screenshot and automation tools cannot find its
window. To capture it regardless:

```bash
SUP="$HOME/Library/Application Support/EmpireEarthMac"
"$SUP/patches/macos/ee-splash-probe"     # prints: wine id=<N> 1024x768
screencapture -x -l<N> /tmp/game.png
```

Logs live in `~/Library/Application Support/EmpireEarthMac/logs`, and the proxy's
own log is `ee-ddraw.log` in the game folder.
