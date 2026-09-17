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
  MUSIC_ENABLED="0"
  # Defaults are the configuration that reaches the main menu (16 Sep 2026).
  # The virtual desktop is load-bearing, not cosmetic: without it Wine's macOS
  # driver minimizes the exclusive-fullscreen window, DXVK reports device-lost,
  # D7VK returns DDERR_SURFACELOST from Flip, and the game re-enumerates for
  # ever on a black screen.  1024x801 is the mode the game's menu actually asks
  # for (SetDisplayMode 1024x801x32), so the desktop matches it exactly.
  VIRTUAL_DESKTOP="1"
  VIRTUAL_DESKTOP_SIZE="1024x801"
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
}
for src, dest in keys.items():
    value = data.get(src, "")
    if isinstance(value, bool):
        value = "1" if value else "0"
    print(f"{dest}={shlex.quote(str(value or ''))}")
PY
)"
  EE_GRAPHICS="${EE_GRAPHICS:-${GRAPHICS_STACK:-d7vk}}"
}

save_config() {
  mkdir -p "$SUPPORT_DIR"
  python3 - "$CONFIG_FILE" \
    "${GAME_DIR:-}" "${BASE_EXE:-}" "${AOC_EXE:-}" \
    "${MUSIC_ENABLED:-0}" "${VIRTUAL_DESKTOP:-0}" \
    "${VIRTUAL_DESKTOP_SIZE:-1024x801}" \
    "${EE_GRAPHICS:-dgvoodoo}" <<'PY'
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
}
with open(path, "w") as handle:
    json.dump(payload, handle, indent=2)
    handle.write("\n")
PY
}

ensure_rosetta() {
  if [[ "$(uname -m)" != "arm64" ]]; then
    return 0
  fi
  if /usr/bin/pgrep oahd >/dev/null 2>&1; then
    return 0
  fi
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
  # dgVoodoo ddraw (D3D7→D3D11) + DXVK-macOS d3d11. dxgi must be native and sit
  # next to d3d11.dll; builtin DXGI is skipped when the game-dir d3d11 import fails.
  export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-mscoree,mshtml=,dsound=builtin,ddraw=native,d3dimm=native,d3d11=native,d3d10core=native,dxgi=native,d3d9=builtin}"
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

game_running() {
  # comm= is enough for a normal launch. Under a virtual desktop the Unix
  # command name is explorer.exe, so fall back to matching the Windows command
  # line. The [ ] / \. keep each grep from matching its own argv.
  if ps -axo comm= | grep -q 'Empire Earth.exe'; then return 0; fi
  if ps -axo comm= | grep -q 'EE-AOC.exe'; then return 0; fi
  if ps -axo args= | grep -q 'Empire[ ]Earth\.exe'; then return 0; fi
  if ps -axo args= | grep -q 'EE-AOC\.exe'; then return 0; fi
  return 1
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
