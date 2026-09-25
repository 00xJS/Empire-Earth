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

# The DLL overrides live in the registry (apply-launch-patches.sh writes them per
# graphics stack).  This variable used to repeat them joined with commas, but
# Wine splits WINEDLLOVERRIDES on ';', so each string parsed as a single entry
# for mscoree/mshtml and every other override in it was ignored.  All it ever
# did -- and all it needs to do -- is keep Wine's .NET and HTML engines off.
export WINEDLLOVERRIDES="mscoree,mshtml="
case "${EE_GRAPHICS:-d7vk}" in
  dxmt)
    export EMPIRE_EARTH_WINE="${EMPIRE_EARTH_WINE:-$HOME/Library/Application Support/EmpireEarthMac/runtime/Wine Devel.app/Contents/Resources/wine/bin/wine}"
    ;;
  d7vk)
    # D7VK can really change modes, so drop the dgVoodoo-era lies in ee-ddraw.
    # (Both were tested and are neutral here, but the truth is the right default
    # on a stack that can honour it.)
    export EE_DDRAW_REAL_SETMODE=1 EE_DDRAW_REAL_GETMODE=1
    ;;
esac

if ! rosetta_ready; then
  die "Rosetta 2 is not installed, so Wine cannot run (macOS upgrades remove it). Install it with: softwareupdate --install-rosetta --agree-to-license"
fi
if memory_note="$(low_memory_warning)"; then :; else log "WARNING: $memory_note"; fi

configure_fullscreen
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

# The launcher's music checkbox writes "Music Enabled" when it changes
# (set-options.sh); otherwise the game owns it (Options > Music Quality, saved on
# exit), so it is not re-applied on every Play -- that undid the game's own
# choice.  Apply it once for installs from before 23 Sep 2026, whose launches
# forced music off whatever the checkbox said.
if [[ ! -f "$SUPPORT_DIR/.music-left-to-game" ]]; then
  "$SCRIPT_DIR/set-options.sh" --apply-registry >/dev/null && touch "$SUPPORT_DIR/.music-left-to-game"
fi
"$SCRIPT_DIR/apply-launch-patches.sh" >/dev/null
# Mac Driver (RetinaMode) is read when wineserver starts. Restart so it applies.
stop_prefix_wine
sleep 1
load_config
if [[ "${EE_FORCE_VIRTUAL_DESKTOP:-0}" == "1" ]]; then
  VIRTUAL_DESKTOP=1
  VIRTUAL_DESKTOP_SIZE="${EE_VIRTUAL_DESKTOP_SIZE:-${VIRTUAL_DESKTOP_SIZE:-1440x933}}"
fi
# One source of truth for "how big is the screen" -- the win32 shims used to
# hard-code 800x600 and tell the game its display was smaller than it is.
export EE_SCREEN_SIZE="${EE_SCREEN_SIZE:-${VIRTUAL_DESKTOP_SIZE:-1440x933}}"
case "$MODE" in
  base|ee) exe="${BASE_EXE:-}" ;;
  aoc|expansion) exe="${AOC_EXE:-}" ;;
esac
if [[ -z "$exe" || ! -f "$exe" ]]; then
  die "$(friendly_missing_game)"
fi
game_dir="$(cd "$(dirname "$exe")" && pwd)"
exe_name="$(basename "$exe")"
# watch-splash.py reads the d7vk proxy's log to tell a hang from a slow load.
if [[ "${EE_GRAPHICS:-d7vk}" == "d7vk" ]]; then
  export EE_DDRAW_LOG="$game_dir/ee-ddraw.log"
fi

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
# Until 22 Sep 2026 about 36% of launches died within seconds in Wine's
# 32<->64-bit thunks (page fault at wow64cpu.dll+0x123d reading 0x00004dc9, or
# a silent exit): Rosetta landing the thunk in the wrong CPU mode.
# install_wow64cpu_rosetta_patch fixes the cause.  Retrying stays as a cheap
# safety net for anything similar: a launch that dies -- crashes, exits, or
# stalls (watch-splash.py: no progress in the proxy log for 60 s, a hang seen a
# few launches in ten on 22 Sep 2026) -- before it has rendered a single frame
# has lost nothing, so start over.  Only a launch that is still visibly making
# progress when the menu timeout runs out is reported without a retry.
early_launch_death() {
  local f="$1"
  [[ -f "$f" ]] || return 1
  grep -qE 'Unhandled page fault|Wine Program Error detected|Game process (crashed|exited|stalled) before' "$f" 2>/dev/null ||
    return 1
  # ee-ddraw.log is rewritten by every run, so this is this attempt's record.
  # Ten frames, as in watch-splash.py: a game hung after its first one has
  # lost nothing either.
  [[ "$(grep -c 'BeginScene ENTER' "$game_dir/ee-ddraw.log" 2>/dev/null)" -lt 10 ]]
}

# Each failed attempt now costs seconds (watch-splash.py returns as soon as the
# game dies), so a handful of attempts is cheap.
launch_attempts="${EE_LAUNCH_ATTEMPTS:-6}"
attempt=1
while :; do

log_file="$LOG_DIR/launch-$(date '+%Y%m%d-%H%M%S').log"
if [[ "$attempt" -gt 1 ]]; then
  log "Launch attempt $attempt of $launch_attempts"
fi

# The proxy reopens ee-ddraw.log when the game loads it; until then the old
# run's "BeginScene" would read as this run's first frame.
[[ -n "${EE_DDRAW_LOG:-}" ]] && rm -f "$EE_DDRAW_LOG"
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
  failure_message="$label crashed while starting (Wine error). See $log_file"
elif grep -q 'stalled before its first frame' "$log_file" 2>/dev/null; then
  launch_failed=1
  failure_message="$label stalled while starting. See $log_file"
elif grep -q 'Still on the opening screen' "$log_file" 2>/dev/null; then
  launch_failed=1
  failure_message="$label did not leave the opening screen within ${splash_timeout}s. See $log_file"
elif [[ "$splash_status" -ne 0 ]]; then
  launch_failed=1
  failure_message="$label exited before leaving the opening screen. See $log_file"
else
  # The splash probe can return a moment before the game settles; give it a
  # few seconds before calling it gone.
  for _ in 1 2 3 4 5; do
    game_running && break
    sleep 1
  done
  if ! game_running; then
    echo "Game process exited before the main menu." >>"$log_file"
    launch_failed=1
    failure_message="$label exited before the main menu. See $log_file"
  fi
fi

if [[ "$launch_failed" == "1" ]]; then
  stop_prefix_wine
  if early_launch_death "$log_file" && [[ "$attempt" -lt "$launch_attempts" ]]; then
    log "Game died before its first frame; retrying"
    pkill -9 -f 'winedbg --' >/dev/null 2>&1 || true
    sleep 2
    attempt=$((attempt + 1))
    continue
  fi
  die "$failure_message"
fi

break
done

echo "$label is running. Close the game window when you are done."
