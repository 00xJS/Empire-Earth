#!/usr/bin/env bash
# Switch the Mac display to 800x600 when that mode already exists.
# Does not install RDM/BetterDisplay or create fake modes.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"

TOOL="$SUPPORT_DIR/patches/macos/ee-display-mode"
SRC="$(cd "$SCRIPT_DIR/.." && pwd)/patches/macos/ee-display-mode.c"

if [[ ! -x "$TOOL" && -f "$SRC" ]]; then
  mkdir -p "$(dirname "$TOOL")"
  clang -O2 -framework CoreGraphics -framework CoreFoundation \
    -o "$TOOL" "$SRC" 2>"$LOG_DIR/ee-display-mode.build.log" || true
fi

if [[ ! -x "$TOOL" ]]; then
  log "Display-mode helper is not available"
  exit 0
fi

if "$TOOL" --list 2>/dev/null | grep -qx '800x600'; then
  "$TOOL" || true
else
  log "This display does not offer 800x600. Fake modes need RDM or BetterDisplay."
  "$TOOL" --list 2>/dev/null | head -20 | while read -r line; do
    log "display mode $line"
  done || true
fi
