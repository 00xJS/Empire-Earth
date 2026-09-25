#!/usr/bin/env bash
# Remember a local GOG folder or Empire Earth.exe / EE-AOC.exe (a GOG setup_*.exe
# is handed to install-game.sh, which installs it into the prefix first).

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"

path="${1:-}"
[[ -n "$path" ]] || die "$(friendly_missing_game)"

load_config

if looks_like_installer "$path"; then
  exec "$SCRIPT_DIR/install-game.sh" "$path"
fi

if ! discover_game "$path"; then
  die "$(friendly_missing_game)"
fi

save_config
echo "$GAME_DIR"
