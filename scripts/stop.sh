#!/usr/bin/env bash
# Panic button.  If the game ever leaves you stuck behind a full-screen window
# with the mouse captured, run this from any terminal:
#
#   ./scripts/stop.sh
#
# It stops the game first, then anything Wine left behind.
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh" 2>/dev/null || true
set +e

# Ask the wineserver to end every process in the prefix first.  SIGKILLing the
# server itself (what this script used to do) orphans Wine's service processes,
# and under Rosetta some of them then hang in the kernel mid-exit until the Mac
# reboots -- 16 of them after one evening of test launches on 22 Sep 2026.
for server in \
  "$RUNTIME_DIR/Wine Stable.app/Contents/Resources/wine/bin/wineserver" \
  "$RUNTIME_DIR/Wine Devel.app/Contents/Resources/wine/bin/wineserver"; do
  [[ -x "$server" ]] && WINEPREFIX="$PREFIX" "$server" -k >/dev/null 2>&1
done
sleep 2

# Anything still alive: the game (matched by its Windows path, so our own
# `wine reg add ...\Empire Earth.exe\...` setup commands never match), then
# winedbg and the virtual desktop's explorer, then the server.
pkill -9 -f '^[A-Za-z]:\\.*\\(Empire Earth|EE-AOC)\.exe' >/dev/null 2>&1
pkill -9 -f 'winedbg' >/dev/null 2>&1
pkill -f 'explorer.exe /desktop' >/dev/null 2>&1
if pgrep -f wineserver >/dev/null 2>&1; then
  sleep 1
  pkill -9 -f wineserver >/dev/null 2>&1
fi

if [[ -n "$(game_pids 2>/dev/null)" ]]; then
  echo "Empire Earth is still running -- run this again, or 'sudo pkill -9 -f wine'." >&2
  exit 1
fi
echo "Empire Earth stopped; the screen is yours again."
