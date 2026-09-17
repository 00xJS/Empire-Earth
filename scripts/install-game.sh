#!/usr/bin/env bash
# Silently install a GOG / Windows Empire Earth setup.exe into the Wine prefix.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"

installer="${1:-}"
if [[ -z "$installer" || ! -f "$installer" ]]; then
  die "Choose your GOG Empire Earth setup .exe (for example setup_empire_earth_gold_*.exe)."
fi
installer="$(cd "$(dirname "$installer")" && pwd)/$(basename "$installer")"

if [[ ! -f "$READY_STAMP" ]]; then
  "$SCRIPT_DIR/setup-prefix.sh"
fi

wine_path="$(require_wine)"
wine_env "$wine_path"

dir_win='C:\GOG Games\Empire Earth Gold Edition'
log "Installing $(basename "$installer") into $dir_win"
set +e
"$wine_path" "$installer" /SILENT /LANG=en /SP- /NOCANCEL /SUPPRESSMSGBOXES /NOGUI /DIR="$dir_win"
status=$?
set -e

if [[ $status -ne 0 ]]; then
  log "Silent install returned $status; retrying with /VERYSILENT"
  set +e
  "$wine_path" "$installer" /VERYSILENT /NORESTART /DIR="$dir_win"
  status=$?
  set -e
fi

if [[ $status -ne 0 ]]; then
  die "The installer exited with status $status. Run it again from the launcher, or install on Windows and choose the copied game folder instead."
fi

search_root="$PREFIX/drive_c"
if ! discover_game "$search_root"; then
  die "Install finished but Empire Earth.exe was not found in the Wine prefix."
fi
save_config
log "Installed game at $GAME_DIR"
echo "$BASE_EXE"
