#!/usr/bin/env bash
# Shared paths and helpers for the Empire Earth Mac launcher.
# These scripts never copy or redistribute game files.

set -euo pipefail

export PATH="/opt/homebrew/bin:/usr/local/bin:/opt/homebrew/sbin:/usr/sbin:/usr/bin:/bin:/sbin:${PATH:-}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SUPPORT_DIR="${EMPIRE_EARTH_SUPPORT_DIR:-$HOME/Library/Application Support/EmpireEarthMac}"
PREFIX="${EMPIRE_EARTH_WINEPREFIX:-$SUPPORT_DIR/wineprefix}"
LOG_DIR="$SUPPORT_DIR/logs"
CONFIG_FILE="$SUPPORT_DIR/config.json"
RUNTIME_DIR="$SUPPORT_DIR/runtime"
WINE_APP="$RUNTIME_DIR/Wine Devel.app"
READY_STAMP="$PREFIX/.ee_mac_ready"
# Installer still fetches Gcenx devel. Graphics currently work better on Stable 11.0
# (11.17's winevulkan/MoltenVK fails DXVK device create on this game).
WINE_VERSION="${EMPIRE_EARTH_WINE_VERSION:-11.17}"
WINE_TARBALL="wine-devel-${WINE_VERSION}-osx64.tar.xz"
WINE_URL="${EMPIRE_EARTH_WINE_URL:-https://github.com/Gcenx/macOS_Wine_builds/releases/download/${WINE_VERSION}/${WINE_TARBALL}}"
WINE_SHA256="${EMPIRE_EARTH_WINE_SHA256:-c2b3a8274dbc594deaa64e40469b607cbc4aa8ef5656dec4c5f6f3dac0da770c}"

mkdir -p "$SUPPORT_DIR" "$LOG_DIR" "$RUNTIME_DIR"

log() {
  local ts
  ts="$(date '+%Y-%m-%d %H:%M:%S')"
  echo "[$ts] $*" | tee -a "$LOG_DIR/launcher.log"
}

die() {
  echo "error: $*" >&2
  log "ERROR: $*"
  exit 1
}

load_config() {
  GAME_DIR=""
  BASE_EXE=""
  AOC_EXE=""
  # Music is on unless the launcher's checkbox (or the game's Options > Music
  # Quality) turns it off: it plays through native DirectMusic (23 Sep 2026).
  MUSIC_ENABLED="1"
  # Defaults are the configuration that reaches the main menu (16 Sep 2026).
  # The virtual desktop is load-bearing, not cosmetic: without it Wine's macOS
  # driver minimizes the exclusive-fullscreen window, DXVK reports device-lost,
  # D7VK returns DDERR_SURFACELOST from Flip, and the game re-enumerates for
  # ever on a black screen.  The desktop is 33 rows taller than the game's own
  # mode (1440x900) because the game's window takes a constant +33px vertical
  # offset when it changes mode -- without those rows the bottom of the main
  # menu, including Exit Game, falls off the desktop.
  VIRTUAL_DESKTOP="1"
  VIRTUAL_DESKTOP_SIZE="1440x933"
  GRAPHICS_STACK="d7vk"
  if [[ ! -f "$CONFIG_FILE" ]]; then
    EE_GRAPHICS="${EE_GRAPHICS:-dgvoodoo}"
    return 0
  fi
  eval "$(python3 - "$CONFIG_FILE" <<'PY'
import json, os, shlex, sys
path = sys.argv[1]
try:
    data = json.load(open(path))
except Exception:
    sys.exit(0)
keys = {
    "game_dir": "GAME_DIR",
    "base_exe": "BASE_EXE",
    "aoc_exe": "AOC_EXE",
    "music_enabled": "MUSIC_ENABLED",
    "virtual_desktop": "VIRTUAL_DESKTOP",
    "virtual_desktop_size": "VIRTUAL_DESKTOP_SIZE",
    "graphics_stack": "GRAPHICS_STACK",
    "fullscreen": "FULLSCREEN",
    "game_resolution": "GAME_RESOLUTION",
}
for src, dest in keys.items():
    value = data.get(src, "")
    if isinstance(value, bool):
        value = "1" if value else "0"
    print(f"{dest}={shlex.quote(str(value or ''))}")
PY
)"
  EE_GRAPHICS="${EE_GRAPHICS:-${GRAPHICS_STACK:-d7vk}}"
  # Keys missing from an older config.json load as empty strings.
  MUSIC_ENABLED="${MUSIC_ENABLED:-1}"
  FULLSCREEN="${FULLSCREEN:-1}"
  GAME_RESOLUTION="${GAME_RESOLUTION:-auto}"
}

save_config() {
  mkdir -p "$SUPPORT_DIR"
  python3 - "$CONFIG_FILE" \
    "${GAME_DIR:-}" "${BASE_EXE:-}" "${AOC_EXE:-}" \
    "${MUSIC_ENABLED:-0}" "${VIRTUAL_DESKTOP:-0}" \
    "${VIRTUAL_DESKTOP_SIZE:-1440x933}" \
    "${EE_GRAPHICS:-dgvoodoo}" "${FULLSCREEN:-1}" "${GAME_RESOLUTION:-auto}" <<'PY'
import json, sys
path = sys.argv[1]
payload = {
    "game_dir": sys.argv[2],
    "base_exe": sys.argv[3],
    "aoc_exe": sys.argv[4],
    "music_enabled": sys.argv[5] in {"1", "true", "True", "yes"},
    "virtual_desktop": sys.argv[6] in {"1", "true", "True", "yes"},
    "virtual_desktop_size": sys.argv[7] or "1920x1080",
    "graphics_stack": sys.argv[8] or "dgvoodoo",
    "fullscreen": sys.argv[9] in {"1", "true", "True", "yes"},
    "game_resolution": sys.argv[10] or "auto",
}
with open(path, "w") as handle:
    json.dump(payload, handle, indent=2)
    handle.write("\n")
PY
}

# Full screen (the default; `set-options.sh --fullscreen off` restores the old
# window).  How Wine 11 on macOS presents a DirectDraw game: when the game takes
# exclusive full screen, wined3d gives its window a "present rect" equal to the
# monitor, and win32u then sizes the game's Mac window to the whole monitor no
# matter what the Windows-side window rect says (win32u apply_window_pos).  In
# a virtual desktop the monitor is the mode the game set.  So:
#   - matches run at the display's own size ("auto"), so the game's window IS
#     the screen, edge to edge, and the mouse maps 1:1;
#   - by default there is no virtual desktop: Wine emulates the game's mode
#     changes (EmulateModeset, see below), so the fixed 1024x768 menu is scaled
#     to the full screen height with black bars at the sides;
#   - EE_EMULATE_MODESET=0 goes back to a virtual desktop the size of the Mac
#     display, where the menu shows 1:1 at the top-left over a black backdrop.
# The menu bar and Dock are hidden while the game is in front (ee-vkfix).
# Trying to centre a smaller window on a full-size monitor does not work: the
# Mac window stays monitor-sized with the picture in its corner (22 Sep 2026).
# Exports the variables apply-launch-patches.sh and ee-vkfix read.
configure_fullscreen() {
  local want="${EE_FULLSCREEN:-${FULLSCREEN:-1}}" res="${EE_GAME_RESOLUTION:-${GAME_RESOLUTION:-auto}}"
  local info src bin w h top below gw gh
  case "$want" in 1|true|on|yes) ;; *) export EE_FULLSCREEN=0; return 0 ;; esac
  src="$SCRIPT_DIR/../patches/macos/ee-screen-info.m"
  bin="$SUPPORT_DIR/patches/macos/ee-screen-info"
  if [[ -f "$src" && ( ! -x "$bin" || "$bin" -ot "$src" ) ]]; then
    mkdir -p "$(dirname "$bin")"
    clang -O2 -fobjc-arc -framework AppKit -o "$bin" "$src" 2>"$LOG_DIR/ee-screen-info.build.log" || true
  fi
  if ! info="$("$bin" 2>/dev/null)" || [[ -z "$info" ]]; then
    log "Full screen: could not read the display size; using the window"
    export EE_FULLSCREEN=0
    return 0
  fi
  read -r w h top below <<<"$info"
  if [[ "$res" =~ ^([0-9]+)x([0-9]+)$ ]]; then
    gw="${BASH_REMATCH[1]}" gh="${BASH_REMATCH[2]}"
  elif [[ "${below:-0}" -gt 0 ]]; then
    # A camera notch: play in the Mac's own below-notch mode (1512x945 on a 14"
    # MacBook Pro).  The MoltenVK shim pins that shorter picture to the bottom
    # of the screen, so the notch covers only black instead of the middle of
    # the game's top bar.  --game-resolution WxH (e.g. the full 1512x982) wins.
    gw="$w" gh="$below"
  else
    gw="$w" gh="$h"
  fi
  export EE_FULLSCREEN=1
  export EE_SCREEN_SIZE="${w}x${h}" EE_SAFE_TOP="$top"
  export EE_GAME_WIDTH="$gw" EE_GAME_HEIGHT="$gh"
  # Emulated modes (the default since 22 Sep 2026; EE_EMULATE_MODESET=0 turns
  # them off): no virtual desktop.  win32u emulates the game's mode changes
  # (EmulateModeset) and maps a full-screen window at a smaller emulated mode
  # onto the whole display, scaled by one ratio and centred (map_monitor_rect),
  # so the fixed 1024x768 menu fills the screen height with black bars at the
  # sides instead of sitting 1:1 in the top-left corner.
  if [[ "${EE_EMULATE_MODESET:-1}" == "1" ]]; then
    export EE_EMULATE_MODESET=1 EE_FORCE_VIRTUAL_DESKTOP=0
    log "Full screen: display ${w}x${h}, emulated modes (menu scaled to fit), matches at ${gw}x${gh}"
    return 0
  fi
  export EE_FORCE_VIRTUAL_DESKTOP=1 EE_VIRTUAL_DESKTOP_SIZE="${w}x${h}"
  log "Full screen: display ${w}x${h}, matches at ${gw}x${gh}"
}

# Can this Mac run x86_64 code?  Actually try it: `pgrep oahd` only says whether
# Rosetta's daemon happens to be running right now (it starts on demand), and a
# major macOS upgrade removes Rosetta outright -- macOS 27 did, on 17 Sep 2026,
# and every launch then failed with "bad CPU type in executable".
rosetta_ready() {
  [[ "$(uname -m)" != "arm64" ]] && return 0
  /usr/bin/arch -x86_64 /usr/bin/true >/dev/null 2>&1
}

# Empire Earth needs well under 1 GB, but it needs it resident.  On 22 Sep 2026
# a 16 GB Mac with 4.7 GB of swap in use and ~70 MB free took seven minutes to
# start a random map: the game's own pages were swapped out as fast as it
# touched them (42 million page faults; 131 MB of its ~200 MB Windows heap
# swapped out), and the render thread sat for minutes in one memcpy.  The
# kernel still called that "normal" pressure, so look at swap and free pages.
# Prints a warning (and returns 1) when the Mac looks that starved.
low_memory_warning() {
  local swap_mb free_mb
  swap_mb="$(sysctl -n vm.swapusage 2>/dev/null | sed -n 's/.*used = \([0-9]*\).*/\1/p')"
  free_mb="$(vm_stat 2>/dev/null | awk '/page size of/ {ps=$8} /Pages free/ {f=$3} /Pages speculative/ {s=$3}
    END {if (ps) printf "%d", (f + s) * ps / 1048576}')"
  [[ -n "$swap_mb" && -n "$free_mb" ]] || return 0
  if (( swap_mb >= 2048 && free_mb < 1024 )); then
    echo "Your Mac is low on memory (${swap_mb} MB swapped out, ${free_mb} MB free). Empire Earth will still run, but starting a match can take several minutes. Quit other apps (browsers, editors, the iOS Simulator) for fast loading."
    return 1
  fi
  return 0
}

ensure_rosetta() {
  rosetta_ready && return 0
  log "Installing Rosetta 2 (free Apple translation layer)"
  /usr/sbin/softwareupdate --install-rosetta --agree-to-license
}

wine_bin_from_app() {
  local app="$1"
  local candidate
  for candidate in \
    "$app/Contents/Resources/wine/bin/wine" \
    "$app/Contents/Resources/wine/bin/wine64"; do
    if [[ -x "$candidate" ]]; then
      echo "$candidate"
      return 0
    fi
  done
  return 1
}

find_wine() {
  local candidate app
  # Honor EMPIRE_EARTH_WINE only. A leftover WINE= from the parent shell
  # previously forced Wine Devel 11.17 and broke DXVK device create.
  if [[ -n "${EMPIRE_EARTH_WINE:-}" && -x "$EMPIRE_EARTH_WINE" ]]; then
    echo "$EMPIRE_EARTH_WINE"
    return 0
  fi
  # Prefer Wine 11.0 (Stable): it is the only stack that has created a D3D11 device
  # for this game. Devel 11.17 is kept as fallback.
  for app in \
    "$RUNTIME_DIR/Wine Stable.app" \
    "$RUNTIME_DIR/Wine Devel.app" \
    "$RUNTIME_DIR/Wine Staging.app" \
    "/Applications/Wine Stable.app" \
    "/Applications/Wine Devel.app" \
    "/Applications/Wine Staging.app"; do
    if candidate="$(wine_bin_from_app "$app")"; then
      echo "$candidate"
      return 0
    fi
  done
  for candidate in "$(command -v wine 2>/dev/null || true)" "$(command -v wine64 2>/dev/null || true)"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then
      echo "$candidate"
      return 0
    fi
  done
  return 1
}

require_wine() {
  local wine_path
  if ! wine_path="$(find_wine)"; then
    die "Wine is not installed. Use the launcher’s Install Wine button, or run scripts/install-wine.sh. Wine is free; no extra purchase is required."
  fi
  echo "$wine_path"
}

wine_env() {
  local wine_path="$1"
  local wine_dir
  wine_dir="$(dirname "$wine_path")"
  export WINEPREFIX="$PREFIX"
  export WINEDEBUG="${WINEDEBUG:--all,+err}"
  # DLL overrides live in the registry (apply-launch-patches.sh); see launch.sh.
  export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-mscoree,mshtml=}"
  export MVK_CONFIG_LOG_LEVEL="${MVK_CONFIG_LOG_LEVEL:-0}"
  export MVK_CONFIG_RESUME_LOST_DEVICE="${MVK_CONFIG_RESUME_LOST_DEVICE:-1}"
  # DXVK's own logging was 100,534 identical warn lines per run
  # ("BlitToDDrawSurface: Failed to lock surface" and its pair).  Each one is a
  # stderr write from the 32-bit process, i.e. an NtWriteFile and a 32->64
  # wow64 transition -- the same dispatcher whose fault kills ~36% of launches.
  # `warn` is not enough; the spam is warn-level.  DXVK_LOG_LEVEL=info restores it.
  export DXVK_HUD="${DXVK_HUD:-0}"
  export DXVK_LOG_LEVEL="${DXVK_LOG_LEVEL:-error}"
  export DXVK_LOG_PATH="${DXVK_LOG_PATH:-$LOG_DIR}"
  # Empty debugger: do not leave a "Program Error" / winedbg window holding the process.
  export WINEDEBUGGER="${WINEDEBUGGER:-}"
  export WINE="$wine_path"
  export WINELOADER="$wine_path"
  export WINESERVER="$wine_dir/wineserver"
  export PATH="$wine_dir:/opt/homebrew/bin:/usr/local/bin:/opt/homebrew/sbin:/usr/sbin:/usr/bin:/bin:/sbin:${PATH:-}"
  # Do not force WINEARCH=win32. Wine 11 on Mac uses a 64-bit WoW64 prefix that can run 32-bit games.
  unset WINEARCH || true
}

# PIDs of live game processes.  Three traps, each of which made a launch
# report the wrong thing:
#   - The game's command line *starts* with its Windows path (C:\...\Empire
#     Earth.exe).  A bare substring match also hits `wine reg add ...\AppDefaults\
#     Empire Earth.exe\...` from our own setup.
#   - A Rosetta process SIGKILLed after a crash can stay in the process table,
#     stuck mid-exit with ~8 KB resident, for minutes.  That is what made every
#     retry after a crash wait out the whole menu timeout.  Require real memory.
#   - No `ps | grep -q`: under `set -o pipefail` an early grep -q match can
#     SIGPIPE ps and turn "found it" into failure, so launch.sh declared a
#     running game dead.
game_pids() {
  ps -axo pid=,rss=,args= | awk '
    { pid = $1; rss = $2; $1 = ""; $2 = ""; sub(/^  /, "") }
    rss > 1024 && $0 ~ /^[A-Za-z]:\\.*\\(Empire Earth|EE-AOC)\.exe/ { print pid }'
}

game_running() {
  [[ -n "$(game_pids)" ]]
}

log_has_usable_swapchain() {
  local log="$1" last
  [[ -f "$log" ]] || return 1
  last="$(grep -E 'Buffer size:' "$log" 2>/dev/null | tail -1 || true)"
  [[ -n "$last" ]] || return 1
  # Still the MoltenVK 1x1 exclusive-fullscreen stall.
  [[ "$last" != *'1x1'* ]] || return 1
  echo "$last" | grep -Eq 'Buffer size:[[:space:]]*[1-9][0-9]{2,}x[1-9][0-9]{2,}'
}

stop_prefix_wine() {
  local wineserver exe pid comm
  # Wine sets comm to the Win32 path, e.g. C:\GOG Games\...\Empire Earth.exe
  while read -r pid comm; do
    case "$comm" in
      *Empire\ Earth.exe*|*EE-AOC.exe*|*winedbg.exe*|*ee-resize.exe*)
        kill -9 "$pid" 2>/dev/null || true
        ;;
    esac
  done < <(ps -axo pid=,comm=)
  for exe in \
    "$RUNTIME_DIR/Wine Stable.app/Contents/Resources/wine/bin/wineserver" \
    "$RUNTIME_DIR/Wine Devel.app/Contents/Resources/wine/bin/wineserver" \
    "$RUNTIME_DIR/Wine Staging.app/Contents/Resources/wine/bin/wineserver"; do
    if [[ -x "$exe" ]]; then
      WINEPREFIX="$PREFIX" "$exe" -k 2>/dev/null || true
    fi
  done
  if wineserver="$(find_wine 2>/dev/null)" && [[ -n "$wineserver" ]]; then
    wineserver="$(dirname "$wineserver")/wineserver"
    if [[ -x "$wineserver" ]]; then
      WINEPREFIX="$PREFIX" "$wineserver" -k 2>/dev/null || true
    fi
  fi
}

log_has_wine_crash() {
  local log="$1"
  [[ -f "$log" ]] || return 1
  grep -qE 'wine: Unhandled|starting debugger|Unhandled page fault|Program Error' "$log"
}

wine_debugger_running() {
  # Do not match osascript snippets that merely mention winedbg.
  pgrep -f 'winedbg --' >/dev/null 2>&1
}

wine_error_window_open() {
  # The Wine "Program Error" / "Wine Debugger" dialogs keep the process alive.
  # Detect them by window title, not just by whether Empire Earth.exe is running.
  python3 - <<'PY'
import subprocess, sys
script = '''
tell application "System Events"
  set out to ""
  try
    if exists process "wine" then
      set out to out & (name of every window of process "wine" as text)
    end if
  end try
  try
    if exists process "winedbg" then
      set out to out & (name of every window of process "winedbg" as text)
    end if
  end try
  return out
end tell
'''
try:
    out = subprocess.check_output(["osascript", "-e", script], timeout=4, text=True)
except Exception:
    sys.exit(1)
low = out.lower()
sys.exit(0 if ("program error" in low or "wine debugger" in low) else 1)
PY
}

find_winetricks() {
  local candidate
  candidate="$(command -v winetricks 2>/dev/null || true)"
  if [[ -n "$candidate" ]]; then
    echo "$candidate"
    return 0
  fi
  return 1
}

directmusic_present() {
  local dll
  for dll in \
    "$PREFIX/drive_c/windows/syswow64/dmusic.dll" \
    "$PREFIX/drive_c/windows/system32/dmusic.dll"; do
    if [[ -f "$dll" ]]; then
      return 0
    fi
  done
  return 1
}

find_named_exe() {
  local root="$1"
  local name="$2"
  if [[ ! -e "$root" ]]; then
    return 1
  fi
  python3 - "$root" "$name" <<'PY'
import os, sys
root, name = sys.argv[1], sys.argv[2].lower()
if os.path.isfile(root) and os.path.basename(root).lower() == name:
    print(root)
    sys.exit(0)
if not os.path.isdir(root):
    sys.exit(1)
hits = []
for dirpath, dirnames, filenames in os.walk(root):
    dirnames[:] = [d for d in dirnames if d not in {".git", "windows", "windows.old"}]
    for filename in filenames:
        if filename.lower() == name:
            hits.append(os.path.join(dirpath, filename))
if hits:
    # Prefer a path that looks like the GOG layout.
    hits.sort(key=lambda p: (("gog" not in p.lower()), len(p)))
    print(hits[0])
    sys.exit(0)
sys.exit(1)
PY
}

discover_game() {
  local root="$1"
  local base aoc parent
  if [[ -f "$root" ]]; then
    parent="$(cd "$(dirname "$root")" && pwd)"
    case "$(basename "$root" | tr '[:upper:]' '[:lower:]')" in
      "empire earth.exe")
        BASE_EXE="$root"
        GAME_DIR="$parent"
        AOC_EXE="$(find_named_exe "$(dirname "$parent")" "ee-aoc.exe" || true)"
        ;;
      "ee-aoc.exe")
        AOC_EXE="$root"
        GAME_DIR="$parent"
        BASE_EXE="$(find_named_exe "$(dirname "$parent")" "empire earth.exe" || true)"
        ;;
      *)
        return 1
        ;;
    esac
    return 0
  fi
  if [[ ! -d "$root" ]]; then
    return 1
  fi
  root="$(cd "$root" && pwd)"
  base="$(find_named_exe "$root" "empire earth.exe" || true)"
  aoc="$(find_named_exe "$root" "ee-aoc.exe" || true)"
  if [[ -z "$base" && -z "$aoc" ]]; then
    return 1
  fi
  BASE_EXE="$base"
  AOC_EXE="$aoc"
  if [[ -n "$base" ]]; then
    GAME_DIR="$(cd "$(dirname "$base")" && pwd)"
  else
    GAME_DIR="$(cd "$(dirname "$aoc")" && pwd)"
  fi
}

looks_like_installer() {
  local path="$1"
  local base
  base="$(basename "$path" | tr '[:upper:]' '[:lower:]')"
  [[ -f "$path" ]] || return 1
  [[ "$base" == *.exe ]] || return 1
  case "$base" in
    setup*.exe|install*.exe|*setup*.exe|*installer*.exe) return 0 ;;
  esac
  return 1
}

friendly_missing_game() {
  echo "No Empire Earth.exe in that folder. Select your GOG Empire Earth Gold folder (it contains Empire Earth.exe), or choose the GOG setup .exe to install into the Wine prefix."
}
