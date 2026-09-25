#!/usr/bin/env bash
# Download Gcenx's free Wine Stable 11.0_1 build into Application Support (lib.sh
# says why that build). Does nothing when a matching Wine is already installed.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"

ensure_rosetta

if [[ -n "${EMPIRE_EARTH_WINE:-}" && -x "$EMPIRE_EARTH_WINE" ]]; then
  log "Using EMPIRE_EARTH_WINE=$EMPIRE_EARTH_WINE"
  echo "$EMPIRE_EARTH_WINE"
  exit 0
fi

# Only our own copy counts, even if another Wine is installed: install-compat-patches.sh
# patches this one (wow64cpu, win32u, MoltenVK), never a Wine elsewhere on the Mac.
wanted="wine-${WINE_VERSION%%_*}"
if wine_path="$(wine_bin_from_app "$WINE_APP")"; then
  current="$("$wine_path" --version 2>/dev/null || true)"
  if [[ "$current" == "$wanted"* || "$current" == "wine-${WINE_VERSION}"* ]]; then
    log "Wine already available: $wine_path ($current)"
    echo "$wine_path"
    exit 0
  fi
  log "Replacing $current in $(basename "$WINE_APP") with $WINE_CHANNEL $WINE_VERSION"
fi

archive="$RUNTIME_DIR/$WINE_TARBALL"
archive_sha256() { shasum -a 256 "$archive" | awk '{print $1}'; }
# A complete earlier download (say, the extract step failed) is reused.
if [[ -f "$archive" && "$(archive_sha256)" == "$WINE_SHA256" ]]; then
  log "Using the already downloaded $WINE_TARBALL"
else
  log "Downloading free Wine $WINE_CHANNEL $WINE_VERSION (Gcenx / WineHQ Mac build)"
  curl -L --fail --retry 3 --progress-bar -o "$archive" "$WINE_URL"
  actual="$(archive_sha256)"
  if [[ "$actual" != "$WINE_SHA256" ]]; then
    rm -f "$archive"
    die "Wine download checksum mismatch (expected $WINE_SHA256, got $actual)"
  fi
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
rm -rf "$extract_dir" "$archive"
xattr -cr "$WINE_APP" 2>/dev/null || true

wine_path="$(wine_bin_from_app "$WINE_APP" || true)"
if [[ -z "$wine_path" ]]; then
  die "Wine installed but the wine binary was not found inside $(basename "$WINE_APP")"
fi

log "Wine installed at $wine_path ($("$wine_path" --version))"
echo "$wine_path"
