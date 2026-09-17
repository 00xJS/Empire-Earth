#!/usr/bin/env bash
# Launch Empire Earth or Art of Conquest from the game directory.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"

MODE="${1:-base}"
unset EE_USE_DGVOODOO || true
unset WINE || true
unset WINELOADER || true
load_config

case "${EE_GRAPHICS:-dgvoodoo}" in
  gog-d3d9)
    export WINEDLLOVERRIDES="mscoree,mshtml=,dsound=builtin,ddraw=native,d3d9=native,dxgi=native,d3d11=builtin,d3d10core=builtin"
    ;;
  dgvoodoo-wined3d)
    export WINEDLLOVERRIDES="mscoree,mshtml=,dsound=builtin,ddraw=native,d3dimm=native,d3d11=builtin,d3d10core=builtin,dxgi=builtin,d3d9=builtin"
    ;;
  dxmt)
    export WINEDLLOVERRIDES="mscoree,mshtml=,dsound=builtin,ddraw=native,d3dimm=native,d3d11=native,d3d10core=native,dxgi=native,winemetal=native,d3d9=builtin"
    export EMPIRE_EARTH_WINE="${EMPIRE_EARTH_WINE:-$HOME/Library/Application Support/EmpireEarthMac/runtime/Wine Devel.app/Contents/Resources/wine/bin/wine}"
    ;;
  d7vk)
    # ddraw=native,builtin is load-bearing: D7VK proxies DirectDrawEnumerate* to
    # Wine's builtin ddraw in syswow64. With a bare ddraw=native it logs
    # "Failed to load proxied ddraw.dll" and the rasterizer scan dies early.
    export WINEDLLOVERRIDES="mscoree,mshtml=,dsound=builtin,ddraw=native,builtin,d3d9=native,d3dimm=builtin,d3d11=builtin,d3d10core=builtin,dxgi=builtin,wined3d=builtin"
    # D7VK can really change modes, so drop the dgVoodoo-era lies in ee-ddraw.
    # (Both were tested and are neutral here, but the truth is the right default
    # on a stack that can honour it.)
    export EE_DDRAW_REAL_SETMODE=1 EE_DDRAW_REAL_GETMODE=1
    ;;
  d3dmetal)
    export WINEDLLOVERRIDES="mscoree,mshtml=,dsound=builtin,ddraw=native,d3dimm=native,d3d11=native,d3d10core=native,dxgi=native,d3d9=builtin"
    ;;
  *)
    export WINEDLLOVERRIDES="mscoree,mshtml=,dsound=builtin,ddraw=native,d3dimm=native,d3d11=native,d3d10core=native,dxgi=native,d3d9=builtin"
    ;;
esac

wine_path="$(require_wine)"
wine_env "$wine_path"

if [[ ! -f "$READY_STAMP" ]] || ! directmusic_present; then
  die "The Wine prefix is not set up yet. Click Set Up Prefix first."
fi

case "$MODE" in
  base|ee)
    exe="${BASE_EXE:-}"
    label="Empire Earth"
    ;;
  aoc|expansion)
    exe="${AOC_EXE:-}"
    label="Empire Earth: The Art of Conquest"
    if [[ -z "$exe" ]]; then
      die "Art of Conquest was not found. Select the GOG Gold folder that contains EE-AOC.exe."
    fi
    if [[ -z "${BASE_EXE:-}" ]]; then
      die "Launch the base game once before Art of Conquest."
    fi
    ;;
  *)
    die "Unknown launch mode '$MODE' (use base or aoc)"
    ;;
esac

if [[ -z "$exe" || ! -f "$exe" ]]; then
  die "$(friendly_missing_game)"
fi

game_dir="$(cd "$(dirname "$exe")" && pwd)"
exe_name="$(basename "$exe")"
log_file="$LOG_DIR/launch-$(date '+%Y%m%d-%H%M%S').log"

stop_prefix_wine
sleep 1

"$SCRIPT_DIR/set-options.sh" --apply-registry >/dev/null
"$SCRIPT_DIR/apply-launch-patches.sh" >/dev/null
# Mac Driver (RetinaMode) is read when wineserver starts. Restart so it applies.
stop_prefix_wine
sleep 1
load_config
if [[ "${EE_FORCE_VIRTUAL_DESKTOP:-0}" == "1" ]]; then
  VIRTUAL_DESKTOP=1
  VIRTUAL_DESKTOP_SIZE="${EE_VIRTUAL_DESKTOP_SIZE:-${VIRTUAL_DESKTOP_SIZE:-1024x801}}"
fi
case "$MODE" in
  base|ee) exe="${BASE_EXE:-}" ;;
  aoc|expansion) exe="${AOC_EXE:-}" ;;
esac
if [[ -z "$exe" || ! -f "$exe" ]]; then
  die "$(friendly_missing_game)"
fi
game_dir="$(cd "$(dirname "$exe")" && pwd)"
exe_name="$(basename "$exe")"

log "Launching $label from $game_dir ($("$wine_path" --version 2>/dev/null || echo wine)) graphics=${EE_GRAPHICS:-d7vk}"
cd "$game_dir"

if [[ "${EE_TRY_DISPLAY_MODE:-0}" == "1" ]]; then
  "$SCRIPT_DIR/try-display-mode.sh" >>"$log_file" 2>&1 || true
fi

# Never block Play on wine start /wait. That sat on the splash for 11 minutes
# after DXVK logged 800x600 off-screen. Cap at 60s to leave the opening banner.
# The 60s cap dates from when the game hung forever on the splash. On the d7vk
# stack it now gets through device creation, render-state setup and texture
# loading, which legitimately takes longer. Default stays 60; the ceiling is 300.
# 60s was declaring failure on runs that were merely still loading: the proxy's
# own progress heartbeat showed textures and frames still climbing, and the game
# reached the main menu a minute after launch.sh had already printed an error
# and given up on it. First launches also pay for the DXVK shader state cache.
splash_timeout="${EE_MENU_TIMEOUT:-240}"
if (( splash_timeout > 1200 )); then
  splash_timeout=1200
fi

watch_for_program_error() {
  local log="$1"
  local seen=0
  local deadline now
  now="$(/bin/date +%s)"
  deadline=$((now + splash_timeout))
  while (( $(/bin/date +%s) < deadline )); do
    if wine_debugger_running || log_has_wine_crash "$log" || wine_error_window_open; then
      echo "Wine Program Error detected; killing hung debugger." >>"$log"
      pkill -9 -f 'winedbg --' 2>/dev/null || true
      stop_prefix_wine
      return 0
    fi
    if game_running; then
      seen=1
    elif [[ "$seen" -eq 1 ]]; then
      return 1
    fi
    sleep 1
  done
  return 1
}

# ---------------------------------------------------------------------------
# About 36% of launches die within ~3 s in Wine's own 32<->64 transition
# dispatcher: the log is <=12 lines and ends either silently or with a page
# fault at wow64cpu.dll+0x123d reading 0x00004dc9.  It is environmental -- not
# our code, not the game -- and it is invisible to everything below it, so the
# only honest remedy is to notice the signature and start over.  Measured
# across 115 launch logs: 42 early deaths, ~34 of them this one signature.
#
# Only this signature is retried.  A genuine hang on the opening screen is a
# real failure and still surfaces immediately.
early_launch_death() {
  local f="$1"
  [[ -f "$f" ]] || return 1
  if grep -qE '00004DC9 at address 7BF2123D|Unhandled page fault|Wine Program Error detected' "$f" 2>/dev/null; then
    return 0
  fi
  if grep -q 'Game process exited before leaving the opening screen' "$f" 2>/dev/null &&
     [[ "$(wc -l <"$f")" -le 12 ]]; then
    return 0
  fi
  return 1
}

launch_attempts="${EE_LAUNCH_ATTEMPTS:-3}"
attempt=1
while :; do

log_file="$LOG_DIR/launch-$(date '+%Y%m%d-%H%M%S').log"
if [[ "$attempt" -gt 1 ]]; then
  log "Launch attempt $attempt of $launch_attempts"
fi

set +e
watch_for_program_error "$log_file" &
watcher_pid=$!
if [[ -f "$game_dir/ee-resize.exe" && "${EE_RESIZE_HELPER:-0}" == "1" ]]; then
  "$wine_path" "$game_dir/ee-resize.exe" >>"$log_file" 2>&1 &
fi
# The virtual desktop is applied through the per-application Explorer registry
# key written by apply-launch-patches.sh. Do not launch via
# `explorer /desktop=NAME,WxH exe` -- that starts explorer.exe and never starts
# the game (verified 15 Sep 2026: explorer sat at 0.1% CPU with no game process).
"$wine_path" start /unix "$exe" >>"$log_file" 2>&1
python3 "$SCRIPT_DIR/watch-splash.py" "$log_file" "$splash_timeout"
splash_status=$?
kill "$watcher_pid" 2>/dev/null || true
wait "$watcher_pid" 2>/dev/null || true
set -e

launch_failed=0
if log_has_wine_crash "$log_file" || grep -q 'Wine Program Error detected' "$log_file" 2>/dev/null || wine_error_window_open; then
  launch_failed=1
  failure_message="$label hit a Wine Program Error (crash dialog). See $log_file"
elif [[ "$splash_status" -ne 0 ]] || grep -q 'Still on the opening screen' "$log_file" 2>/dev/null; then
  launch_failed=1
  failure_message="$label did not leave the opening screen within ${splash_timeout}s. See $log_file"
fi

if [[ "$launch_failed" == "1" ]]; then
  stop_prefix_wine
  if early_launch_death "$log_file" && [[ "$attempt" -lt "$launch_attempts" ]]; then
    log "Early-launch crash in Wine's 32/64 dispatcher; retrying"
    pkill -9 -f 'winedbg --' >/dev/null 2>&1 || true
    sleep 2
    attempt=$((attempt + 1))
    continue
  fi
  die "$failure_message"
fi

break
done

# The game can still be starting when the splash probe returns.
for _ in 1 2 3 4 5; do
  if game_running; then
    break
  fi
  sleep 1
done
if ! game_running; then
  die "$label exited before the main menu. See $log_file"
fi

echo "$label is running. Close the game window when you are done."
