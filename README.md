# Empire Earth on macOS

Play **your own GOG copy** of Empire Earth Gold Edition (the base game and The
Art of Conquest) on an Apple Silicon Mac, full screen, using only free tools:
[Wine](https://wiki.winehq.org/MacOS), DXVK and MoltenVK. No CrossOver or
Parallels needed.

This repository is a launcher and a set of compatibility patches. It contains
**no game files**; you supply the game.

## What you get

- Both games in single player: random maps, campaigns, scenarios, saved games
  and the Scenario Editor, with sound and music.
- Full screen at the Mac's own resolution, and `Cmd`+`Tab` in and out. On a
  MacBook with a notch, every screen sits below it.
- Smooth big battles: about 130 FPS on an M2 Pro in a late-game battle with
  over a thousand units moving. Much of the game's maths now runs on modern
  CPU instructions, with results identical to the original.
- Mac keyboard mappings for the keys a Mac lacks (see [Keys](#keys-on-a-mac)).
- Bigger armies: a unit limit of up to 10000, drag-select takes every unit in
  the box, and every unit of a big crowd is drawn.

Multiplayer is not a goal of this project.

## Requirements

- An Apple Silicon Mac with macOS 14 or later
- Empire Earth Gold Edition from GOG: the Windows offline installer
  (`setup_empire_earth_*.exe`) or an already installed game folder
- [Homebrew](https://brew.sh), used to install the free `winetricks` and
  `mingw-w64`
- Rosetta 2 (installed automatically if missing)

Retail CD copies are not supported (their copy protection does not run).

## Install

```bash
chmod +x scripts/*.sh
./scripts/install-wine.sh
./scripts/setup-prefix.sh
./scripts/set-game.sh "/path/to/setup_empire_earth_gold.exe"
```

`install-wine.sh` downloads a free Wine build (about 185 MB) and
`setup-prefix.sh` prepares its Windows environment. `set-game.sh` accepts the
GOG installer or a folder containing `Empire Earth.exe`.

To get the installer: GOG Galaxy for Mac won't install this Windows game, so
open [your GOG account](https://www.gog.com/en/account) in a browser, find
**Empire Earth Gold Edition** and download the **Windows offline backup
installer**.

## Play

```bash
./scripts/launch.sh        # Empire Earth
./scripts/launch.sh aoc    # The Art of Conquest
```

Or build the Mac app and use its Play button:

```bash
./scripts/build-app.sh
open "dist/Empire Earth.app"
```

## Keys on a Mac

| On the Mac | In the game |
|---|---|
| `delete` | Del: kill the selected unit (`Shift`+`delete` kills all selected) |
| `=` and `-` | keypad + and −: game speed up and down |
| `fn`+`F1`…`F12` | the F-keys: `F3` pause, `F4` quick save, `F10` options, `F11` clock and FPS |
| `fn`+`↑` | Page Up: previous messages |
| `Option` | Alt |
| `Control`+`Option`+`Q` | release the mouse and minimize the game |
| `Control`+`Option`+`X` | quit the game |
| `Cmd`+`Tab` | switch apps (the game pauses while it is in the background) |

To use the F-keys without `fn`, turn on **Use F1, F2, etc. keys as standard
function keys** in System Settings → Keyboard → Keyboard Shortcuts → Function
Keys. macOS uses `F11` for Show Desktop and, if you have turned them on,
`Control`+number to switch desktops; turn those shortcuts off in the Mission
Control section of the same window if you want them in the game (`F11`, and
`Control`+number to create groups).

## Bigger armies

The **Game Unit Limit** on the game setup screen now goes up to 10000 (the
original stops at 1200). The limit is shared by all players, and very large
armies make the game slower.

A **drag-select** now takes every unit in the box (the original stops at 63).
This works in single player; for a LAN game, turn it off (below) on every
machine.

**Every unit on screen is drawn.** The original draws at most 512 units at a
time, so in a big crowd the nearest ones blinked in and out.

## Options

Options are environment variables for the launcher, for example:

```bash
EE_BIG_ARMIES=0 ./scripts/launch.sh
```

| Variable | Effect |
|---|---|
| `EE_BIG_ARMIES=0` | the original unit limit (1200) and selection size (63) |
| `EE_BIG_CROWDS=0` | draw at most 512 units on screen, as the original does |
| `EE_MAC_KEYS=0` | the original key bindings only |
| `EE_ANIMATION_SMOOTHING=0` | turn Animation Smoothing off (a few more FPS; units move less smoothly) |
| `EE_X87T=0`, `EE_SSE_MATH=0` | run the game's original maths (slower) |

## If something goes wrong

- A start that hangs before the menu is retried automatically.
- `./scripts/stop.sh` stops the game and everything Wine started.
- Logs are in `~/Library/Application Support/EmpireEarthMac/logs`, and in
  `ee-ddraw.log` and `ee-version.log` in the game folder.

## How it works

Empire Earth is a 32-bit Direct3D 7 game. It runs under Rosetta 2 in Wine, and
its graphics go through a chain of translation layers:

```
Empire Earth.exe → ddraw.dll (this project's proxy) → D7VK (Direct3D 7 → 9)
                 → DXVK (Direct3D 9 → Vulkan) → MoltenVK (Vulkan → Metal)
```

Two small DLLs from this project run inside the game (`patches/win32`): a
DirectDraw proxy that keeps full screen, focus and page flips working, and a
`version.dll` proxy that carries the fixes, the speed-ups and the extras above.
A shim around MoltenVK (`patches/vulkan`) handles the Mac window and full
screen. Each change to the game's code is applied in memory at start-up, and
only after checking that the game's bytes are exactly what it expects.
