#!/usr/bin/env bash
# Remember a local GOG folder or Empire Earth.exe / EE-AOC.exe.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"

path="${1:-}"
[[ -n "$path" ]] || die "$(friendly_missing_game)"

load_config

if looks_like_installer "$path"; then
  die "That file looks like a setup.exe. Use Install from GOG setup instead of Choose folder."
fi

if ! discover_game "$path"; then
  die "$(friendly_missing_game)"
fi

save_config
echo "$GAME_DIR"
