#!/usr/bin/env bash
# Download the free Gcenx Wine devel build into Application Support.
# Devel includes the Rosetta 2 32-bit thunk workaround and DXMT (D3D11→Metal).

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"

ensure_rosetta

wanted="wine-${WINE_VERSION%%_*}"
if wine_path="$(find_wine)"; then
  current="$("$wine_path" --version 2>/dev/null || true)"
  if [[ "$current" == "$wanted"* || "$current" == "wine-${WINE_VERSION}"* ]]; then
    log "Wine already available: $wine_path ($current)"
    echo "$wine_path"
    exit 0
  fi
  log "Upgrading Wine ($current -> $wanted)"
fi

archive="$RUNTIME_DIR/$WINE_TARBALL"
log "Downloading free Wine devel $WINE_VERSION (Gcenx / WineHQ Mac build)"
curl -L --fail --retry 3 --progress-bar -o "$archive" "$WINE_URL"

actual="$(shasum -a 256 "$archive" | awk '{print $1}')"
if [[ "$actual" != "$WINE_SHA256" ]]; then
  rm -f "$archive"
  die "Wine download checksum mismatch (expected $WINE_SHA256, got $actual)"
fi

extract_dir="$RUNTIME_DIR/extract-$$"
mkdir -p "$extract_dir"
tar -xJf "$archive" -C "$extract_dir"
app_src="$(find "$extract_dir" -maxdepth 3 \( -name 'Wine Devel.app' -o -name 'Wine Staging.app' -o -name 'Wine Stable.app' \) -type d | head -1 || true)"
if [[ -z "$app_src" ]]; then
  rm -rf "$extract_dir"
  die "Wine archive did not contain a Wine .app"
fi

rm -rf "$WINE_APP"
mkdir -p "$(dirname "$WINE_APP")"
mv "$app_src" "$WINE_APP"
rm -rf "$extract_dir"
xattr -cr "$WINE_APP" 2>/dev/null || true

wine_path="$(wine_bin_from_app "$WINE_APP" || true)"
if [[ -z "$wine_path" ]]; then
  die "Wine installed but the wine binary was not found inside $(basename "$WINE_APP")"
fi

log "Wine installed at $wine_path ($("$wine_path" --version))"
echo "$wine_path"
