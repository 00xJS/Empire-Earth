#!/usr/bin/env bash
# Fetch Wine-compatible wrappers into Application Support.
# dgVoodoo 2.82+ crashes inside DDraw on Wine when creating a primary surface
# (WineHQ 58731). Pin 2.79.3, the last well-tested pre-2.82 release.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"

DGV_DIR="$SUPPORT_DIR/patches/dgVoodoo-2.79.3"
DGV_ZIP="$SUPPORT_DIR/patches/dgVoodoo2_79_3.zip"
DGV_URL="${EMPIRE_EARTH_DGVOODOO_URL:-https://web.archive.org/web/20221115181022id_/https://github.com/dege-diosg/dgVoodoo2/releases/download/v2.79.3/dgVoodoo2_79_3.zip}"
DGV_SHA256="${EMPIRE_EARTH_DGVOODOO_SHA256:-67e5ff5f647555f5cd39c6cee9ffa2ecadcd7c9f4fe3b75e33c17486223e2d41}"
REPO_PATCHES="$(cd "$SCRIPT_DIR/.." && pwd)/patches"

install_dgvoodoo_279() {
  if [[ -f "$DGV_DIR/DDraw.dll" && -f "$DGV_DIR/D3DImm.dll" ]]; then
    log "dgVoodoo 2.79.3 already installed"
    return 0
  fi
  mkdir -p "$SUPPORT_DIR/patches"
  if [[ ! -f "$DGV_ZIP" ]]; then
    log "Downloading dgVoodoo 2.79.3 (Wine-compatible Direct3D 7 wrapper)"
    curl -L --fail --retry 3 -o "$DGV_ZIP" "$DGV_URL"
  fi
  local actual
  actual="$(shasum -a 256 "$DGV_ZIP" | awk '{print $1}')"
  if [[ "$actual" != "$DGV_SHA256" ]]; then
    rm -f "$DGV_ZIP"
    die "dgVoodoo 2.79.3 checksum mismatch (expected $DGV_SHA256, got $actual)"
  fi
  local extract="$SUPPORT_DIR/patches/dgv-279-extract"
  rm -rf "$extract"
  mkdir -p "$extract" "$DGV_DIR"
  unzip -q "$DGV_ZIP" -d "$extract"
  cp "$extract/MS/x86/DDraw.dll" "$DGV_DIR/DDraw.dll"
  cp "$extract/MS/x86/D3DImm.dll" "$DGV_DIR/D3DImm.dll"
  cp "$extract/dgVoodoo.conf" "$DGV_DIR/dgVoodoo.conf"
  rm -rf "$extract"
  log "Installed dgVoodoo 2.79.3 into $DGV_DIR"
}

copy_repo_configs() {
  mkdir -p "$SUPPORT_DIR/patches/dxvk" "$SUPPORT_DIR/patches/wine"
  if [[ -f "$REPO_PATCHES/dxvk/dxvk.conf" ]]; then
    cp "$REPO_PATCHES/dxvk/dxvk.conf" "$SUPPORT_DIR/patches/dxvk/dxvk.conf"
  fi
  if [[ -f "$REPO_PATCHES/wine/empire-earth.reg" ]]; then
    cp "$REPO_PATCHES/wine/empire-earth.reg" "$SUPPORT_DIR/patches/wine/empire-earth.reg"
  fi
}

apply_wine_reg() {
  local wine_path
  wine_path="$(find_wine || true)"
  [[ -n "$wine_path" && -d "$PREFIX" ]] || return 0
  wine_env "$wine_path"
  local reg="$SUPPORT_DIR/patches/wine/empire-earth.reg"
  [[ -f "$reg" ]] || return 0
  "$wine_path" regedit /S "$reg" >/dev/null 2>&1 || true
  log "Applied Wine compatibility registry patch"
}

install_moltenvk_shim() {
  local wine_lib src real shim stamp
  wine_lib="$RUNTIME_DIR/Wine Stable.app/Contents/Resources/wine/lib"
  src="$REPO_PATCHES/vulkan/ee-vkfix.c"
  [[ -f "$src" && -d "$wine_lib" ]] || return 0
  real="$wine_lib/libMoltenVK.real.dylib"
  shim="$wine_lib/libMoltenVK.dylib"
  stamp="$wine_lib/.ee-vkfix-stamp"
  if [[ ! -f "$wine_lib/libMoltenVK.dylib.wine110" ]]; then
    if [[ -f "$shim" ]] && ! grep -q 'ee-vkfix' <<<"$(strings "$shim" 2>/dev/null || true)"; then
      cp -p "$shim" "$wine_lib/libMoltenVK.dylib.wine110"
    fi
  fi
  if [[ ! -f "$wine_lib/libMoltenVK.dylib.wine110" ]]; then
    log "No original MoltenVK to wrap; skip surface shim"
    return 0
  fi
  if [[ -f "$stamp" && -f "$shim" && "$src" -ot "$stamp" && -f "$real" ]]; then
    return 0
  fi
  cp -p "$wine_lib/libMoltenVK.dylib.wine110" "$real"
  install_name_tool -id '@rpath/libMoltenVK.real.dylib' "$real" 2>/dev/null || true
  if ! clang -arch x86_64 -x objective-c -fobjc-arc -dynamiclib -O2 -mmacosx-version-min=11.0 \
      -install_name '@rpath/libMoltenVK.dylib' \
      -framework AppKit -framework QuartzCore \
      -o "$shim" "$src" \
      -Wl,-reexport_library,"$real" 2>"$LOG_DIR/ee-vkfix-build.log"; then
    log "MoltenVK surface shim failed to link with reexport; trying ICD-only"
    if ! clang -arch x86_64 -x objective-c -fobjc-arc -dynamiclib -O2 -mmacosx-version-min=11.0 \
        -install_name '@rpath/libMoltenVK.dylib' \
        -framework AppKit -framework QuartzCore \
        -o "$shim" "$src" 2>>"$LOG_DIR/ee-vkfix-build.log"; then
      cp -p "$wine_lib/libMoltenVK.dylib.wine110" "$shim"
      log "MoltenVK surface shim build failed; restored original (see $LOG_DIR/ee-vkfix-build.log)"
      return 0
    fi
  fi
  codesign --force --sign - "$real" "$shim" >/dev/null 2>&1 || true
  date >"$stamp"
  log "Installed MoltenVK 1x1 surface shim"
}

# Wine 11 win32u copies HWND GetClientRect over MoltenVK currentExtent. That
# turns a real 800x600 Metal layer into a 1x1 DXGI swapchain. NOP the two
# adjust_surface_capabilities() calls in this Wine 11.0 build.
install_win32u_extent_patch() {
  local so bak stamp
  so="$RUNTIME_DIR/Wine Stable.app/Contents/Resources/wine/lib/wine/x86_64-unix/win32u.so"
  bak="$so.wine110"
  stamp="$so.ee-extent-stamp"
  [[ -f "$so" ]] || return 0
  if [[ ! -f "$bak" ]]; then
    cp -p "$so" "$bak"
  fi
  local rc=0 # set -e: a missing pattern (exit 2) must reach the restore below
  python3 - "$so" "$bak" "$stamp" <<'PY' || rc=$?
from pathlib import Path
import sys
so, bak, stamp = map(Path, sys.argv[1:])
data = bytearray(so.read_bytes())
patterns = [
    bytes.fromhex("4c 89 f7 48 89 de e8 84 6b 00 00 31 c0"),
    bytes.fromhex("4c 89 f7 48 89 de e8 ea 6b 00 00 31 c0"),
]
nops = bytes.fromhex("4c 89 f7 48 89 de 90 90 90 90 90 31 c0")
already = 0
patched = 0
missing = 0
for pat in patterns:
    if data.find(pat) < 0:
        if data.find(nops) >= 0:
            already += 1
        else:
            missing += 1
        continue
    idx = data.find(pat)
    data[idx:idx + len(pat)] = nops
    patched += 1
if missing:
    sys.exit(2)
if patched:
    so.write_bytes(data)
stamp.write_text(f"patched={patched} already={already}\n")
sys.exit(0)
PY
  if [[ $rc -eq 2 ]]; then
    log "win32u extent patch: pattern not found, restoring original"
    cp -p "$bak" "$so"
    return 0
  fi
  if [[ $rc -ne 0 ]]; then
    log "win32u extent patch failed"
    return 0
  fi
  codesign --force --sign - "$so" >/dev/null 2>&1 || true
  log "Patched Wine win32u so Vulkan extents stay at the Metal layer size"
}

# Wine's WoW64 thunks cross between the game's 32-bit code and Wine's 64-bit
# code with far jumps (`jmp far [ptr]` in, `ljmp` out).  Under Rosetta 2, a
# thread crossing while another thread makes Rosetta discard translated code --
# a DLL unloaded, a page re-protected -- can land in the wrong CPU mode.  That
# was the "~36% of launches die in wow64cpu.dll+0x123d" crash, and it also
# killed the first skirmish while the map loaded.  CrossOver fixes it
# (CW HACK 20760: lcall in, lretq out); Gcenx's 11.0_1 build dropped that
# patch, so patches/wine/patch-wow64cpu.py re-applies it to the exact DLL we
# ship against, always starting from the untouched original.
# EE_WOW64CPU_PATCH=0 puts the original back for comparison runs.
install_wow64cpu_rosetta_patch() {
  local dll bak stamp
  dll="$RUNTIME_DIR/Wine Stable.app/Contents/Resources/wine/lib/wine/x86_64-windows/wow64cpu.dll"
  bak="$dll.wine110"
  stamp="$dll.ee-rosetta-stamp"
  [[ -f "$dll" ]] || return 0
  [[ -f "$bak" ]] || cp -p "$dll" "$bak"
  if [[ "${EE_WOW64CPU_PATCH:-1}" == "0" ]]; then
    if ! cmp -s "$dll" "$bak"; then
      cp -p "$bak" "$dll"
      rm -f "$stamp"
      log "Restored the original wow64cpu.dll (EE_WOW64CPU_PATCH=0)"
    fi
    return 0
  fi
  if [[ -f "$stamp" && "$REPO_PATCHES/wine/patch-wow64cpu.py" -ot "$stamp" &&
        "$REPO_PATCHES/wine/wow64cpu-rosetta.S" -ot "$stamp" ]] && ! cmp -s "$dll" "$bak"; then
    return 0
  fi
  if python3 "$REPO_PATCHES/wine/patch-wow64cpu.py" "$bak" "$dll.ee-new" >"$LOG_DIR/wow64cpu-patch.log" 2>&1; then
    mv "$dll.ee-new" "$dll"
    date >"$stamp"
    log "Patched Wine wow64cpu.dll for Rosetta 2 (CrossOver's lcall/lretq mode switch)"
  else
    rm -f "$dll.ee-new"
    log "WARNING: wow64cpu Rosetta patch not applied (see $LOG_DIR/wow64cpu-patch.log)"
  fi
}

install_dgvoodoo_279
# D7VK is the default graphics stack; nothing else fetches it on a first run.
"$SCRIPT_DIR/install-d7vk.sh" >/dev/null
copy_repo_configs
# The three Wine fixes below are written for, and only ever patch, the pinned
# Wine Stable 11.0_1 in the runtime folder -- never a Wine installed elsewhere.
if [[ ! -d "$RUNTIME_DIR/Wine Stable.app" ]]; then
  log "WARNING: $RUNTIME_DIR/Wine Stable.app is missing, so the Rosetta and Vulkan fixes are not applied and the game is likely to crash at start-up. Install it with the launcher's Install Wine button or scripts/install-wine.sh."
fi
install_win32u_extent_patch
install_wow64cpu_rosetta_patch
install_moltenvk_shim
apply_wine_reg
if command -v i686-w64-mingw32-gcc >/dev/null 2>&1; then
  "$SCRIPT_DIR/build-win32-helpers.sh" || log "Win32 DXGI helper build failed"
else
  log "WARNING: MinGW not installed, so the ee-ddraw/ee-version fix-up DLLs cannot be built and the game runs without them (brew install mingw-w64)"
fi
SRC_DISPLAY="$(cd "$SCRIPT_DIR/.." && pwd)/patches/macos/ee-display-mode.c"
if [[ -f "$SRC_DISPLAY" ]]; then
  mkdir -p "$SUPPORT_DIR/patches/macos"
  clang -O2 -framework CoreGraphics -framework CoreFoundation \
    -o "$SUPPORT_DIR/patches/macos/ee-display-mode" "$SRC_DISPLAY" \
    2>"$LOG_DIR/ee-display-mode.build.log" || log "Display-mode helper build failed"
fi
SRC_PROBE="$(cd "$SCRIPT_DIR/.." && pwd)/patches/macos/ee-splash-probe.c"
if [[ -f "$SRC_PROBE" ]]; then
  mkdir -p "$SUPPORT_DIR/patches/macos"
  clang -O2 -framework CoreGraphics -framework CoreFoundation \
    -o "$SUPPORT_DIR/patches/macos/ee-splash-probe" "$SRC_PROBE" \
    2>"$LOG_DIR/ee-splash-probe.build.log" || log "Splash probe build failed"
fi
echo "Compat patches ready: $DGV_DIR"
