#!/usr/bin/env bash
# Panic button.  If the game ever leaves you stuck behind a full-screen window
# with the mouse captured, run this from any terminal:
#
#   ./scripts/stop.sh
#
# It stops the game first, then anything Wine left behind.
set -uo pipefail

stopped=0
for pat in "Empire Earth.exe" "EE-AOC.exe" "winedbg"; do
  if pgrep -f "$pat" >/dev/null 2>&1; then
    pkill -f "$pat" >/dev/null 2>&1 || true
    stopped=1
  fi
done

if [[ "$stopped" == "1" ]]; then
  sleep 2
  for pat in "Empire Earth.exe" "EE-AOC.exe" "winedbg"; do
    pgrep -f "$pat" >/dev/null 2>&1 && pkill -9 -f "$pat" >/dev/null 2>&1 || true
  done
fi

# The explorer that hosts the virtual desktop, and then the server itself.
pkill -f "explorer.exe /desktop" >/dev/null 2>&1 || true
sleep 1
pkill -f wineserver >/dev/null 2>&1 || true
sleep 1
pkill -9 -f wineserver >/dev/null 2>&1 || true
pkill -9 -f winedevice >/dev/null 2>&1 || true

if pgrep -f "Empire Earth.exe" >/dev/null 2>&1; then
  echo "Empire Earth is still running -- run this again, or 'sudo pkill -9 -f wine'." >&2
  exit 1
fi
echo "Empire Earth stopped; the screen is yours again."
