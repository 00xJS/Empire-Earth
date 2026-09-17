#!/usr/bin/env bash
# Create the Wine prefix and install DirectMusic (required to start Empire Earth).

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"

ensure_rosetta
"$SCRIPT_DIR/install-wine.sh" >/dev/null
wine_path="$(require_wine)"
wine_env "$wine_path"

if ! command -v winetricks >/dev/null 2>&1; then
  if ! command -v brew >/dev/null 2>&1; then
    die "Homebrew is required to install winetricks (free). Install Homebrew from https://brew.sh then try again."
  fi
  log "Installing winetricks via Homebrew"
  brew install winetricks
fi
winetricks_bin="$(find_winetricks || true)"
[[ -n "$winetricks_bin" ]] || die "winetricks is not installed"

if [[ -f "$READY_STAMP" ]] && directmusic_present; then
  log "Prefix already ready at $PREFIX"
  echo "Prefix ready: $PREFIX"
  exit 0
fi

mkdir -p "$PREFIX"
log "Initializing Wine prefix at $PREFIX"
if ! "$wine_path" wineboot --init; then
  log "Default prefix init failed; retrying with WINEARCH=win32"
  export WINEARCH=win32
  rm -rf "$PREFIX"
  mkdir -p "$PREFIX"
  "$wine_path" wineboot --init
fi

winetricks_log="$LOG_DIR/winetricks-$(date '+%Y%m%d-%H%M%S').log"
log "Setting Windows 10 compatibility mode"
if ! "$winetricks_bin" -q win10 >>"$winetricks_log" 2>&1; then
  log "winetricks win10 returned a non-zero status; continuing (details in $winetricks_log)"
fi

log "Installing DirectMusic (Empire Earth crashes on start without this)"
if ! "$winetricks_bin" -q directmusic >>"$winetricks_log" 2>&1; then
  die "DirectMusic install failed. See $winetricks_log"
fi

if ! directmusic_present; then
  die "DirectMusic did not appear in the Wine prefix after winetricks. Prefix is not ready."
fi

load_config
MUSIC_ENABLED="${MUSIC_ENABLED:-0}"
"$SCRIPT_DIR/set-options.sh" --apply-registry
touch "$READY_STAMP"
"$SCRIPT_DIR/install-compat-patches.sh" >/dev/null
log "Prefix is ready"
echo "Prefix ready: $PREFIX"
