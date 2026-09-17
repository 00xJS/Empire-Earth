#!/usr/bin/env bash
# Build Win32 DXGI/D3D11 windowed wrappers and the HWND resize helper.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"

CC="${EMPIRE_EARTH_MINGW_CC:-i686-w64-mingw32-gcc}"
SRC="$(cd "$SCRIPT_DIR/.." && pwd)/patches/win32"
OUT="$SUPPORT_DIR/patches/win32"
mkdir -p "$OUT" "$LOG_DIR"

if ! command -v "$CC" >/dev/null 2>&1; then
  die "MinGW is required to build the 800x600 DXGI helper. Install it with: brew install mingw-w64"
fi

build_one() {
  local name="$1"
  shift
  log "Building $name"
  if ! "$CC" -O2 -Wall "$@" -o "$OUT/$name" 2>"$LOG_DIR/${name}.build.log"; then
    cat "$LOG_DIR/${name}.build.log" >&2 || true
    die "Failed to build $name (see $LOG_DIR/${name}.build.log)"
  fi
}

build_one version.dll -shared "$SRC/ee-version.c" "$SRC/ee-version.def" \
  -Wl,--enable-stdcall-fixup -static-libgcc -luser32 -lkernel32
# -ldxguid supplies IID_IDirect3D7 / IID_IDirect3D*Device, which the
# IDirect3D7 instrumentation compares against.
# ee-ddraw.rc gives the proxy a ProductName version resource; D7VK refuses to
# use a proxied ddraw whose ProductName it cannot read.
TMPDIR_RC="$(mktemp -d)"
"${WINDRES:-i686-w64-mingw32-windres}" "$SRC/ee-ddraw.rc" -O coff -o "$TMPDIR_RC/ee-ddraw-res.o"
build_one ddraw.dll -shared "$SRC/ee-ddraw.c" "$TMPDIR_RC/ee-ddraw-res.o" "$SRC/ee-ddraw.def" \
  -Wl,--enable-stdcall-fixup -static-libgcc -luser32 -lkernel32 -ldxguid -lversion
build_one dxgi.dll -shared "$SRC/ee-dxgi.c" "$SRC/ee-dxgi.def" \
  -Wl,--enable-stdcall-fixup -static-libgcc -luser32 -lkernel32
build_one d3d11.dll -shared "$SRC/ee-d3d11.c" "$SRC/ee-d3d11.def" \
  -Wl,--enable-stdcall-fixup -static-libgcc -luser32 -lkernel32
build_one ee-resize.exe "$SRC/ee-resize.c" -luser32 -lkernel32
build_one ee-nudge.exe "$SRC/ee-nudge.c" -luser32 -lkernel32
log "Win32 helpers ready in $OUT"
echo "$OUT"
