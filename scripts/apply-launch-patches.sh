#!/usr/bin/env bash
# Launch-time compatibility patches for Empire Earth on Wine/macOS.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"

load_config
# GRAPHICS_STACK / VIRTUAL_DESKTOP as loaded from disk. EE_GRAPHICS and
# EE_FORCE_VIRTUAL_DESKTOP are per-run overrides and must never be written back
# into config.json, or one experiment silently changes the saved default.
SAVED_GRAPHICS="${GRAPHICS_STACK:-dgvoodoo}"
SAVED_VD="${VIRTUAL_DESKTOP:-0}"
SAVED_VD_SIZE="${VIRTUAL_DESKTOP_SIZE:-1440x933}"
SAVED_MUSIC="${MUSIC_ENABLED:-0}"

save_config_preserving_prefs() {
  local r_g="${EE_GRAPHICS:-}" r_v="${VIRTUAL_DESKTOP:-}"
  local r_s="${VIRTUAL_DESKTOP_SIZE:-}" r_m="${MUSIC_ENABLED:-}"
  EE_GRAPHICS="$SAVED_GRAPHICS"; VIRTUAL_DESKTOP="$SAVED_VD"
  VIRTUAL_DESKTOP_SIZE="$SAVED_VD_SIZE"; MUSIC_ENABLED="$SAVED_MUSIC"
  save_config
  EE_GRAPHICS="$r_g"; VIRTUAL_DESKTOP="$r_v"
  VIRTUAL_DESKTOP_SIZE="$r_s"; MUSIC_ENABLED="$r_m"
}

wine_path="$(require_wine)"
wine_env "$wine_path"
"$SCRIPT_DIR/install-compat-patches.sh" >/dev/null

reg() {
  "$wine_path" reg add "$1" /v "$2" /t "$3" /d "$4" /f >/dev/null 2>&1 || true
}

skip_movies() {
  [[ "${EE_SKIP_MOVIES:-1}" == "1" ]] || return 0
  local root="$1"
  local dir="$root/Data/Movies"
  if [[ -d "$dir" && ! -d "${dir}.skipped" ]]; then
    mv "$dir" "${dir}.skipped"
    mkdir -p "$dir"
    log "Skipped intro movies in $root"
  elif [[ -d "$dir" && -d "${dir}.skipped" ]]; then
    # Movies were restored; skip them again.
    rm -rf "$dir"
    mkdir -p "$dir"
    log "Cleared intro movies in $root"
  fi
  # Keep binkw32.dll loaded. The game can crash at process start if Bink is missing;
  # empty Movies/ is enough to skip Sierra/SSSI/Empire Earth.bik.
  if [[ -f "$root/binkw32.dll.disabled" && ! -f "$root/binkw32.dll" ]]; then
    mv "$root/binkw32.dll.disabled" "$root/binkw32.dll"
    log "Restored Bink DLL in $root"
  fi
}

patch_wonlobby() {
  local cfg="$1/WONLobby.cfg"
  [[ -f "$cfg" ]] || return 0
  python3 - "$cfg" <<'PY'
from pathlib import Path
import sys
path = Path(sys.argv[1])
text = path.read_text(errors="replace")
out = []
for line in text.splitlines():
    stripped = line.lstrip()
    if stripped.startswith("CDKeyCheck:"):
        out.append("CDKeyCheck: false")
    elif stripped.startswith("CheckForInternet:"):
        out.append("CheckForInternet: false")
    else:
        out.append(line)
path.write_text("\n".join(out) + "\n")
PY
  log "Disabled CD key / internet check in $cfg"
}

patch_dxcfg() {
  local ini="$1/dxcfg.ini"
  [[ -f "$ini" ]] || return 0
  python3 - "$ini" <<'PY'
from pathlib import Path
import sys
path = Path(sys.argv[1])
text = path.read_text(errors="replace")
replacements = {
    "display": "desktop",
    "presentation": "windowed",
    "vsync": "off",
}
out = []
for line in text.splitlines():
    if "=" in line and not line.strip().startswith("["):
        key, _, _ = line.partition("=")
        k = key.strip().lower()
        if k in replacements:
            out.append(f"{key.strip()}={replacements[k]}")
            continue
    out.append(line)
path.write_text("\n".join(out) + "\n")
PY
}

dgv="$SUPPORT_DIR/patches/dgVoodoo-2.79.3"
d7vk="$SUPPORT_DIR/patches/d7vk/dxvk-sarek-1.13.0/build/x32"
if [[ ! -f "$dgv/DDraw.dll" ]]; then
  dgv="$SUPPORT_DIR/patches/dgVoodoo"
fi
dxvk_root="$SUPPORT_DIR/patches/dxvk"
dxvk_dxgi_root="$SUPPORT_DIR/patches/dxvk-d3d9"

install_dxvk_into() {
  local dest="$1"
  [[ -f "$dxvk_root/x32/d3d11.dll" ]] || return 0
  [[ -d "$dest" ]] || return 0
  cp "$dxvk_root/x32/d3d11.dll" "$dest/d3d11.dll"
  cp "$dxvk_root/x32/d3d10core.dll" "$dest/d3d10core.dll"
  # Gcenx's d3d11 repack omits dxgi.dll. DXVK d3d11 will not load without it in the game dir.
  if [[ -f "$dxvk_dxgi_root/x32/dxgi.dll" ]]; then
    cp "$dxvk_dxgi_root/x32/dxgi.dll" "$dest/dxgi.dll"
  elif [[ -f "$dxvk_root/x32/dxgi.dll" ]]; then
    cp "$dxvk_root/x32/dxgi.dll" "$dest/dxgi.dll"
  fi
  rm -f "$dest/winemetal.dll" "$dest/d3d9.dll"
  install_windowed_wrappers "$dest"
}

dll_has_marker() {
  grep -q "$2" "$1" 2>/dev/null
}

# Force DXGI/D3D11 swapchains to 800x600 windowed so DXVK does not copy a 1x1 exclusive-FS desc.
install_windowed_wrappers() {
  local dest="$1"
  local helpers="$SUPPORT_DIR/patches/win32"
  [[ -d "$dest" ]] || return 0
  case "${EE_GRAPHICS:-dgvoodoo}" in
    dgvoodoo-wined3d|dxmt|d3dmetal|d7vk) return 0 ;;
  esac
  if [[ ! -f "$helpers/dxgi.dll" ]]; then
    "$SCRIPT_DIR/build-win32-helpers.sh" >/dev/null || true
  fi
  [[ -f "$helpers/dxgi.dll" ]] || return 0

  if [[ -f "$dest/dxgi.dll" ]]; then
    if ! dll_has_marker "$dest/dxgi.dll" "ee-dxgi:"; then
      cp "$dest/dxgi.dll" "$dest/dxgi_eeorig.dll"
    fi
    if [[ -f "$dest/dxgi_eeorig.dll" ]]; then
      cp "$helpers/dxgi.dll" "$dest/dxgi.dll"
    fi
  fi
  if [[ -f "$dest/d3d11.dll" && -f "$helpers/d3d11.dll" ]]; then
    if ! dll_has_marker "$dest/d3d11.dll" "ee-d3d11:"; then
      cp "$dest/d3d11.dll" "$dest/d3d11_eeorig.dll"
    fi
    if [[ -f "$dest/d3d11_eeorig.dll" ]]; then
      cp "$helpers/d3d11.dll" "$dest/d3d11.dll"
    fi
  fi
  if [[ -f "$helpers/ee-resize.exe" ]]; then
    cp "$helpers/ee-resize.exe" "$dest/ee-resize.exe"
  fi
  if [[ -f "$helpers/ee-nudge.exe" ]]; then
    cp "$helpers/ee-nudge.exe" "$dest/ee-nudge.exe"
  fi
  log "Installed 800x600 windowed DXGI wrappers in $dest"
}

install_version_shim() {
  local dest="$1"
  local helpers="$SUPPORT_DIR/patches/win32"
  local orig="$RUNTIME_DIR/Wine Stable.app/Contents/Resources/wine/lib/wine/i386-windows/version.dll"
  [[ -d "$dest" ]] || return 0
  if [[ ! -f "$helpers/version.dll" ]]; then
    "$SCRIPT_DIR/build-win32-helpers.sh" >/dev/null || true
  fi
  [[ -f "$helpers/version.dll" && -f "$orig" ]] || return 0
  cp "$orig" "$dest/version_eeorig.dll"
  cp "$helpers/version.dll" "$dest/version.dll"
  log "Installed early 800x600 display-mode shim (version.dll)"
}

install_dxvk() {
  [[ -f "$dxvk_root/x32/d3d11.dll" ]] || return 0
  mkdir -p "$PREFIX/drive_c/windows/syswow64" "$PREFIX/drive_c/windows/system32"
  cp "$dxvk_root/x32/d3d11.dll" "$PREFIX/drive_c/windows/syswow64/d3d11.dll"
  cp "$dxvk_root/x32/d3d10core.dll" "$PREFIX/drive_c/windows/syswow64/d3d10core.dll"
  if [[ -f "$dxvk_dxgi_root/x32/dxgi.dll" ]]; then
    cp "$dxvk_dxgi_root/x32/dxgi.dll" "$PREFIX/drive_c/windows/syswow64/dxgi.dll"
  fi
  if [[ -f "$dxvk_root/x64/d3d11.dll" ]]; then
    cp "$dxvk_root/x64/d3d11.dll" "$PREFIX/drive_c/windows/system32/d3d11.dll"
    cp "$dxvk_root/x64/d3d10core.dll" "$PREFIX/drive_c/windows/system32/d3d10core.dll"
  fi
  if [[ -f "$dxvk_dxgi_root/x64/dxgi.dll" ]]; then
    cp "$dxvk_dxgi_root/x64/dxgi.dll" "$PREFIX/drive_c/windows/system32/dxgi.dll"
  fi
  log "Installed DXVK-macOS (D3D11 via MoltenVK)"
}

install_dgvoodoo() {
  local dest="$1"
  local src="${EE_DGVOODOO_DIR:-$dgv}"
  [[ -d "$src" && -f "$src/DDraw.dll" ]] || return 0
  [[ -d "$dest" ]] || return 0
  if [[ -f "$dest/ddraw.dll" && ! -f "$dest/ddraw.dll.gogbackup" ]]; then
    # Keep GOG's D3D7→D3D9 wrapper so we can switch back without reinstalling.
    if ! grep -q 'dgVoodoo' "$dest/ddraw.dll" 2>/dev/null; then
      local gog_size
      gog_size="$(wc -c < "$dest/ddraw.dll" | tr -d ' ')"
      if [[ "${gog_size:-0}" -gt 1000000 ]]; then
        cp "$dest/ddraw.dll" "$dest/ddraw.dll.gogbackup"
      fi
    fi
  fi
  cp "$src/DDraw.dll" "$dest/ddraw_eeorig.dll"
  {
    local helpers="$SUPPORT_DIR/patches/win32"
    if [[ ! -f "$helpers/ddraw.dll" ]]; then
      "$SCRIPT_DIR/build-win32-helpers.sh" >/dev/null || true
    fi
    if [[ -f "$helpers/ddraw.dll" ]] && grep -q 'ee-ddraw:' <<<"$(strings "$helpers/ddraw.dll" 2>/dev/null || true)"; then
      cp "$helpers/ddraw.dll" "$dest/ddraw.dll"
    else
      cp "$src/DDraw.dll" "$dest/ddraw.dll"
    fi
  }
  cp "$src/D3DImm.dll" "$dest/D3DImm.dll"
  # ddraw_.dll is D7VK's proxy target; Empire Earth LoadLibrary-scans every *.dll
  # in this folder, so do not leave it behind on other stacks.
  rm -f "$dest/ddraw_.dll" "$dest/ee_sysddraw.dll"
  if [[ -f "$src/dgVoodoo.conf" ]]; then
    cp "$src/dgVoodoo.conf" "$dest/dgVoodoo.conf"
  else
    cp "$dgv/dgVoodoo.conf" "$dest/dgVoodoo.conf"
  fi
  install_dxvk_into "$dest"
  write_dxvk_conf "$dest"
  patch_dgvoodoo_conf "$dest/dgVoodoo.conf"
  log "Installed dgVoodoo2 + DXVK into $dest"
}

write_dxvk_conf() {
  local dest="$1/dxvk.conf"
  local src="$SCRIPT_DIR/../patches/dxvk/dxvk.conf"
  if [[ -f "$src" ]]; then
    cp "$src" "$dest"
    return 0
  fi
  cat >"$dest" <<'EOF'
dxgi.deferSurfaceCreation = False
dxgi.syncInterval = 0
dxgi.maxFrameRate = 60
dxgi.tearFree = False
d3d11.relaxedBarriers = True
EOF
}

patch_dgvoodoo_conf() {
  local conf="$1"
  [[ -f "$conf" ]] || return 0
  python3 - "$conf" <<'PY'
from pathlib import Path
import sys
path = Path(sys.argv[1])
lines = path.read_text(errors="replace").splitlines()
updates = {
    "General": {
        "OutputAPI": "d3d11_fl11_0",
        "FullScreenMode": "false",
        "CenterAppWindow": "true",
        "CaptureMouse": "false",
        "ScalingMode": "stretched_ar",
        "InheritColorProfileInFullScreenMode": "false",
    },
    "GeneralExt": {
        "FPSLimit": "60",
        "WindowedAttributes": "borderless",
        "PresentationModel": "discard",
        "FullscreenAttributes": "fake",
        "EnableGDIHooking": "true",
        "DesktopResolution": "800x600",
        "DesktopBitDepth": "32",
    },
    "DirectX": {
        "Resolution": "800x600",
        "AppControlledScreenMode": "false",
        "ForceVerticalSync": "true",
        "VideoCard": "internal3D",
        "dgVoodooWatermark": "false",
    },
    "DirectXExt": {
        "DeferredScreenModeSwitch": "false",
        "DefaultEnumeratedResolutions": "classics",
        "ExtraEnumeratedResolutions": "800x600,1024x768,1440x900",
        "EnumeratedResolutionBitdepths": "16,32",
        "PrimarySurfaceBatchedUpdate": "true",
    },
}
section = ""
out = []
for line in lines:
    stripped = line.strip()
    if stripped.startswith("[") and stripped.endswith("]"):
        section = stripped[1:-1]
        out.append(line)
        continue
    if "=" in line and not stripped.startswith(";"):
        key = line.split("=", 1)[0].strip()
        if section in updates and key in updates[section]:
            out.append(f"{key} = {updates[section][key]}")
            continue
    out.append(line)
path.write_text("\n".join(out) + "\n")
PY
}

install_dgvoodoo_wined3d() {
  local dest="$1"
  install_dgvoodoo "$dest"
  rm -f "$dest/d3d11.dll" "$dest/d3d10core.dll" "$dest/dxgi.dll" "$dest/d3d9.dll" "$dest/winemetal.dll"
  log "Using Wine builtin D3D11 (wined3d) under dgVoodoo in $dest"
}

install_dxmt_into() {
  local dest="$1"
  local dxmt="$SUPPORT_DIR/patches/dxmt/v0.80/i386-windows"
  [[ -f "$dxmt/d3d11.dll" ]] || die "DXMT 0.80 i386 d3d11.dll is not installed under $dxmt"
  install_dgvoodoo "$dest"
  cp "$dxmt/d3d11.dll" "$dest/d3d11.dll"
  cp "$dxmt/d3d10core.dll" "$dest/d3d10core.dll"
  cp "$dxmt/dxgi.dll" "$dest/dxgi.dll"
  cp "$dxmt/winemetal.dll" "$dest/winemetal.dll"
  rm -f "$dest/dxgi_eeorig.dll" "$dest/d3d11_eeorig.dll"
  log "Installed DXMT d3d11/dxgi/winemetal under dgVoodoo in $dest"
}

install_d3dmetal() {
  local dest="$1"
  local root="${EE_D3DMETAL_DIR:-$HOME/Library/Application Support/Wineskin/Wrapper/Wineskin-3.0.6.app/Contents/Frameworks/d3dmetal}"
  local x32="" cand
  for cand in "$root/wine/i386-windows" "$root/i386-windows"; do
    if [[ -f "$cand/d3d11.dll" && -f "$cand/dxgi.dll" ]]; then
      x32="$cand"
      break
    fi
  done
  if [[ -z "$x32" ]]; then
    die "D3DMetal/GPTK on this Mac is 64-bit only (no i386 d3d11.dll). Empire Earth is a 32-bit D3D7 game, so Apple's D3DMetal cannot wrap it. Use the default dgVoodoo + DXVK stack."
  fi
  install_dgvoodoo "$dest"
  cp "$x32/d3d11.dll" "$dest/d3d11.dll"
  cp "$x32/dxgi.dll" "$dest/dxgi.dll"
  [[ -f "$x32/d3d10core.dll" ]] && cp "$x32/d3d10core.dll" "$dest/d3d10core.dll"
  rm -f "$dest/dxgi_eeorig.dll" "$dest/d3d11_eeorig.dll"
  log "Installed D3DMetal i386 d3d11/dxgi under dgVoodoo in $dest"
}

install_gog_d3d9() {
  local dest="$1"
  [[ -d "$dest" ]] || return 0
  if [[ -f "$dest/ddraw.dll.gogbackup" ]]; then
    cp "$dest/ddraw.dll.gogbackup" "$dest/ddraw.dll"
  fi
  [[ -f "$dxvk_dxgi_root/x32/d3d9.dll" ]] || return 0
  cp "$dxvk_dxgi_root/x32/d3d9.dll" "$dest/d3d9.dll"
  cp "$dxvk_dxgi_root/x32/dxgi.dll" "$dest/dxgi.dll"
  rm -f "$dest/d3d11.dll" "$dest/d3d10core.dll" "$dest/D3DImm.dll" "$dest/winemetal.dll"
  write_dxvk_conf "$dest"
  install_windowed_wrappers "$dest"
  log "Installed GOG ddraw (D3D7→D3D9) + DXVK d3d9 into $dest"
}

# D7VK: a native Direct3D 7 -> Vulkan implementation (DXVK lineage), shipped by
# DXVK-Sarek built against a Vulkan 1.1/1.2 baseline -- which is exactly what
# MoltenVK provides. It replaces dgVoodoo, D3DImm, DXVK-d3d11 and DXGI with one
# 32-bit ddraw.dll, and it exposes a real "D7VK T&L HAL" device, which is the
# rasterizer Empire Earth actually asks for.
#
# Empire Earth LoadLibrary-scans EVERY *.dll in the game folder alphabetically
# during startup, so any wrapper DLL we leave behind gets probed as a plugin.
# Remove the whole dgVoodoo/DXVK chain rather than layering on top of it.
install_d7vk() {
  local dest="$1"
  if [[ ! -f "$d7vk/ddraw.dll" ]]; then
    die "D7VK is not installed. Run ./scripts/install-d7vk.sh first."
  fi
  [[ -d "$dest" ]] || return 0
  if [[ -f "$dest/ddraw.dll" && ! -f "$dest/ddraw.dll.gogbackup" ]]; then
    local sz; sz="$(wc -c < "$dest/ddraw.dll" | tr -d ' ')"
    [[ "${sz:-0}" -gt 1000000 ]] && cp "$dest/ddraw.dll" "$dest/ddraw.dll.gogbackup"
  fi
  rm -f "$dest/D3DImm.dll" "$dest/d3d11.dll" "$dest/d3d10core.dll" "$dest/dxgi.dll" \
        "$dest/ddraw_eeorig.dll" "$dest/d3d11_eeorig.dll" "$dest/dxgi_eeorig.dll" \
        "$dest/dgVoodoo.conf" "$dest/winemetal.dll"
  # EE_D7VK_D3D9=<file> swaps the D3D9 backend under D7VK. D7VK's ddraw only
  # imports Direct3DCreate9, so any d3d9 that implements it will do -- which
  # makes the Vulkan backend independently testable.
  if [[ -n "${EE_D7VK_D3D9:-}" && -f "${EE_D7VK_D3D9}" ]]; then
    cp "$EE_D7VK_D3D9" "$dest/d3d9.dll"
    log "Installed d3d9 backend from EE_D7VK_D3D9=$EE_D7VK_D3D9"
  else
    cp "$d7vk/d3d9.dll"  "$dest/d3d9.dll"
  fi
  # D7VK implements Direct3D7 over D3D9 but delegates DirectDrawCreateEx and
  # DirectDrawEnumerate* to a real "<sysdir>\ddraw.dll". Wine's loader keys
  # modules by BASE NAME, so that request comes back to D7VK itself and it logs
  # "CreateDirectDrawEx: Failed to load proxied ddraw.dll" -- enumeration yields
  # nothing and Empire Earth then null-derefs in Low-Level Engine.dll+0x6FE8.
  #
  # Interposing the ee-ddraw proxy (EE_D7VK_TRACE=1) to serve those re-entrant
  # calls was tried on 15 Sep and made things worse: with the proxy and a copy of
  # Wine's builtin ddraw in the process, D7VK stops initialising altogether (no
  # "LOADING D7VK" line at all). So D7VK is installed bare by default; the trace
  # layout is opt-in and currently only useful for logging the API sequence.
  # THE unlock. D7VK delegates DirectDrawCreateEx / DirectDrawEnumerate* to a
  # real ddraw, and GetProxiedDDrawModule() tries LoadLibraryA("ddraw_.dll") from
  # the app directory BEFORE falling back to "<sysdir>\ddraw.dll". That fallback
  # is fatal under Wine, whose loader keys modules by BASE NAME and so hands D7VK
  # itself ("Failed to load proxied ddraw.dll") -- zero adapters, and Empire
  # Earth null-derefs at Low-Level Engine.dll+0x6FE8.
  # Dropping Wine's builtin ddraw in as ddraw_.dll takes D7VK's own escape hatch
  # and the whole chain comes up: D3D7 HAL device, D3D9/Vulkan, main window.
  if [[ -f "$PREFIX/drive_c/windows/syswow64/ddraw.dll" ]]; then
    cp "$PREFIX/drive_c/windows/syswow64/ddraw.dll" "$dest/ddraw_.dll"
  else
    log "WARNING: no syswow64 ddraw.dll to install as ddraw_.dll; D7VK will fail"
  fi
  # The ee-ddraw proxy is now the DEFAULT layout for this stack, not a debug
  # option: with it, Empire Earth creates its main window at 800x600 (instead of
  # the 1512x982 desktop size) and IDirect3D7::EnumDevices reports all three D7VK
  # devices including the T&L HAL. EE_D7VK_BARE=1 drops back to raw D7VK.
  local helpers="$SUPPORT_DIR/patches/win32"
  # Rebuild the proxy when its source is newer than the built copy, so an
  # edited ee-ddraw.c takes effect at the next launch (the Vulkan shim does the
  # same in launch.sh).
  local proxy_src="$SCRIPT_DIR/../patches/win32/ee-ddraw.c"
  if [[ "${EE_D7VK_BARE:-0}" != "1" && -f "$proxy_src" && \
        ( ! -f "$helpers/ddraw.dll" || "$helpers/ddraw.dll" -ot "$proxy_src" ) ]]; then
    "$SCRIPT_DIR/build-win32-helpers.sh" >/dev/null || log "WARNING: could not rebuild the ee-ddraw proxy"
  fi
  if [[ "${EE_D7VK_BARE:-0}" != "1" && -f "$helpers/ddraw.dll" ]]; then
    cp "$d7vk/ddraw.dll"    "$dest/ddraw_eeorig.dll"
    cp "$helpers/ddraw.dll" "$dest/ddraw.dll"
    log "Installed D7VK (DXVK-Sarek 1.13.0) + ee-ddraw proxy into $dest"
  else
    rm -f "$dest/ddraw_eeorig.dll" "$dest/ee_sysddraw.dll"
    cp "$d7vk/ddraw.dll" "$dest/ddraw.dll"
    log "Installed D7VK (DXVK-Sarek 1.13.0), bare, into $dest"
  fi
  # The dxvk.conf shipped for the dgVoodoo stack is all d3d11/dxgi keys and means
  # nothing to Sarek's d3d9. Write one that matters here.
  #
  # shaderCompilationMethod: "dyasync" (DXVK-Sarek's default) compiles a
  # pipeline in the background while drawing with the closest variant that is
  # already compiled, and compiles on the spot only when there is none.  Until
  # 23 Sep 2026 this was "async", which draws NOTHING until the pipeline is
  # ready: the first frame of every menu screen came out black, and once the
  # proxy emulated real page flipping that frame stayed on one of the two pages
  # (flicker, black squares around the cursor).  "async" was chosen on 16 Sep
  # against a first-frame hang ("main thread parks while the compiler threads
  # churn") that predates the wow64cpu, focus and Vulkan-memory fixes;
  # dyasync ran the menus and a match cleanly on 23 Sep.
  # EE_DXVK_CONF=<file> installs that config instead of the default, so the
  # present-related keys can be bisected one at a time without editing this
  # script between runs.
  if [[ -n "${EE_DXVK_CONF:-}" && -f "${EE_DXVK_CONF}" ]]; then
    cp "$EE_DXVK_CONF" "$dest/dxvk.conf"
    log "Installed dxvk.conf from EE_DXVK_CONF=$EE_DXVK_CONF"
    return 0
  fi
  cat > "$dest/dxvk.conf" <<'DXVKCONF'
# DXVK-Sarek (D7VK) knobs for Empire Earth -- D3D7 -> D3D9 -> Vulkan -> Metal.
#
# The game clears, flips (hr=0) and then blocks inside the wrapper at 0% CPU on
# the first frame's GPU work, leaving a full-screen black window. A present that
# waits on a vblank MoltenVK never delivers looks exactly like that, so vsync is
# off and frame latency is minimal; forceLegacyPresent/legacyPresentGuard are
# D7VK's own escape hatches for the Flip path.
dxvk.shaderCompilationMethod = dyasync
dxvk.enableStateCache = True
dxvk.numCompilerThreads = 4
d3d9.presentInterval = 0
d3d9.maxFrameLatency = 1
# ddraw.forceLegacyPresent is deliberately NOT set: bisected 16 Sep, it makes
# the game crash with a Wine Program Error.
#
# ddraw.legacyPresentGuard is deliberately NOT set either.  "True" is not one of
# the tri-state's accepted values (strict|disabled|auto), so it silently fell
# through to Auto and OVERRODE the built-in Empire Earth profile's Strict --
# which made D7VK present from inside every Blt, producing 6,596 swapchain
# recreations, a 101k-line log and a 100% CPU spin.  Bisected 16 Sep.
DXVKCONF
  log "Wrote d7vk dxvk.conf (async shader compilation)"
}

install_graphics() {
  local dest="$1"
  case "${EE_GRAPHICS:-dgvoodoo}" in
    gog-d3d9)
      install_gog_d3d9 "$dest"
      ;;
    dgvoodoo-wined3d)
      install_dgvoodoo_wined3d "$dest"
      ;;
    dxmt)
      install_dxmt_into "$dest"
      ;;
    d3dmetal)
      install_d3dmetal "$dest"
      ;;
    d7vk)
      install_d7vk "$dest"
      ;;
    *)
      install_dgvoodoo "$dest"
      ;;
  esac
}

# The old "virtual desktop == exclusive fullscreen == 1x1 swapchain" problem was
# fixed by the DXGI proxy, so the virtual desktop is a real option again and is
# no longer forced off here. EE_FORCE_VIRTUAL_DESKTOP=1 turns it on for one run.
if [[ "${EE_FORCE_VIRTUAL_DESKTOP:-0}" == "1" ]]; then
  VIRTUAL_DESKTOP=1
  VIRTUAL_DESKTOP_SIZE="${EE_VIRTUAL_DESKTOP_SIZE:-${VIRTUAL_DESKTOP_SIZE:-1440x933}}"
fi
MUSIC_ENABLED=0

reg "HKCU\\Software\\Wine\\DllOverrides" "ddraw" "REG_SZ" "native"
reg "HKCU\\Software\\Wine\\DllOverrides" "dsound" "REG_SZ" "builtin"
reg "HKCU\\Software\\Wine\\DllOverrides" "dmusic" "REG_SZ" "native"
if [[ "${EE_GRAPHICS:-dgvoodoo}" == "gog-d3d9" ]]; then
  reg "HKCU\\Software\\Wine\\DllOverrides" "d3d9" "REG_SZ" "native"
  reg "HKCU\\Software\\Wine\\DllOverrides" "dxgi" "REG_SZ" "native"
  reg "HKCU\\Software\\Wine\\DllOverrides" "d3d11" "REG_SZ" "builtin"
  reg "HKCU\\Software\\Wine\\DllOverrides" "d3d10core" "REG_SZ" "builtin"
  reg "HKCU\\Software\\Wine\\DllOverrides" "wined3d" "REG_SZ" "builtin"
elif [[ "${EE_GRAPHICS:-dgvoodoo}" == "dgvoodoo-wined3d" ]]; then
  reg "HKCU\\Software\\Wine\\DllOverrides" "d3dimm" "REG_SZ" "native"
  reg "HKCU\\Software\\Wine\\DllOverrides" "d3d11" "REG_SZ" "builtin"
  reg "HKCU\\Software\\Wine\\DllOverrides" "dxgi" "REG_SZ" "builtin"
  reg "HKCU\\Software\\Wine\\DllOverrides" "d3d10core" "REG_SZ" "builtin"
  reg "HKCU\\Software\\Wine\\DllOverrides" "d3d9" "REG_SZ" "builtin"
  reg "HKCU\\Software\\Wine\\DllOverrides" "wined3d" "REG_SZ" "builtin"
elif [[ "${EE_GRAPHICS:-dgvoodoo}" == "dxmt" ]]; then
  reg "HKCU\\Software\\Wine\\DllOverrides" "d3dimm" "REG_SZ" "native"
  reg "HKCU\\Software\\Wine\\DllOverrides" "d3d11" "REG_SZ" "native"
  reg "HKCU\\Software\\Wine\\DllOverrides" "dxgi" "REG_SZ" "native"
  reg "HKCU\\Software\\Wine\\DllOverrides" "d3d10core" "REG_SZ" "native"
  reg "HKCU\\Software\\Wine\\DllOverrides" "winemetal" "REG_SZ" "native"
  reg "HKCU\\Software\\Wine\\DllOverrides" "d3d9" "REG_SZ" "builtin"
  reg "HKCU\\Software\\Wine\\DllOverrides" "wined3d" "REG_SZ" "builtin"
elif [[ "${EE_GRAPHICS:-dgvoodoo}" == "d7vk" ]]; then
  # D7VK's ddraw.dll has NO Vulkan symbols -- it is a D3D7->D3D9 layer that
  # imports Direct3DCreate9. The Vulkan backend is DXVK-Sarek's d3d9.dll. If
  # d3d9 is left builtin, D7VK lands on Wine's wined3d, Direct3DCreate9 gives it
  # nothing usable and DirectDrawCreateEx returns 0x80004005 with no log line.
  reg "HKCU\\Software\\Wine\\DllOverrides" "d3d9" "REG_SZ" "native"
  reg "HKCU\\Software\\Wine\\DllOverrides" "d3dimm" "REG_SZ" "builtin"
  reg "HKCU\\Software\\Wine\\DllOverrides" "d3d11" "REG_SZ" "builtin"
  reg "HKCU\\Software\\Wine\\DllOverrides" "dxgi" "REG_SZ" "builtin"
  reg "HKCU\\Software\\Wine\\DllOverrides" "d3d10core" "REG_SZ" "builtin"
  reg "HKCU\\Software\\Wine\\DllOverrides" "wined3d" "REG_SZ" "builtin"
elif [[ "${EE_GRAPHICS:-dgvoodoo}" == "d3dmetal" ]]; then
  reg "HKCU\\Software\\Wine\\DllOverrides" "d3dimm" "REG_SZ" "native"
  reg "HKCU\\Software\\Wine\\DllOverrides" "d3d11" "REG_SZ" "native"
  reg "HKCU\\Software\\Wine\\DllOverrides" "dxgi" "REG_SZ" "native"
  reg "HKCU\\Software\\Wine\\DllOverrides" "d3d10core" "REG_SZ" "native"
  reg "HKCU\\Software\\Wine\\DllOverrides" "d3d9" "REG_SZ" "builtin"
  reg "HKCU\\Software\\Wine\\DllOverrides" "wined3d" "REG_SZ" "builtin"
else
  reg "HKCU\\Software\\Wine\\DllOverrides" "d3dimm" "REG_SZ" "native"
  reg "HKCU\\Software\\Wine\\DllOverrides" "d3d11" "REG_SZ" "native"
  reg "HKCU\\Software\\Wine\\DllOverrides" "dxgi" "REG_SZ" "native"
  reg "HKCU\\Software\\Wine\\DllOverrides" "d3d10core" "REG_SZ" "native"
  reg "HKCU\\Software\\Wine\\DllOverrides" "d3d9" "REG_SZ" "builtin"
  reg "HKCU\\Software\\Wine\\DllOverrides" "wined3d" "REG_SZ" "builtin"
fi
# Do not leave a Wine "Program Error" dialog holding a dead process open.
reg "HKCU\\Software\\Wine\\WineDbg" "ShowCrashDialog" "REG_DWORD" "0"

# winecfg -v can open a GUI and hang under the Mac driver. Set XP via registry.
reg "HKCU\\Software\\Wine" "Version" "REG_SZ" "winxp"
"$wine_path" reg add "HKCU\\Software\\Wine\\AppDefaults\\Empire Earth.exe" /v Version /t REG_SZ /d winxp /f >/dev/null 2>&1 || true
reg "HKCU\\Software\\Wine\\AppDefaults\\Empire Earth.exe\\DllOverrides" "version" "REG_SZ" "native"

reg "HKCU\\Software\\Wine\\Direct3D" "VideoMemorySize" "REG_SZ" "2048"
if [[ "${EE_GRAPHICS:-d7vk}" == "dgvoodoo-wined3d" ]]; then
  # D3D11 via wined3d. GL hangs windowless on 11.17; Vulkan/MoltenVK is the other backend.
  reg "HKCU\\Software\\Wine\\Direct3D" "renderer" "REG_SZ" "vulkan"
elif [[ "${EE_GRAPHICS:-d7vk}" == "d7vk" ]]; then
  # Tempting but WRONG: "no3d" here. Wine's own ddraw (ddraw_.dll, D7VK's escape
  # hatch) calls wined3d_create() inside DirectDrawEnumerateExA before it invokes
  # the first adapter callback, and under no3d that returns NULL -- the game then
  # dereferences the empty result and dies with
  #   "page fault on read access to 00000000 at address 00B66FE8"
  # 3 launches out of 3, before D7VK even loads. Tested 17 Sep 2026.
  reg "HKCU\\Software\\Wine\\Direct3D" "renderer" "REG_SZ" "${EE_WINED3D_RENDERER:-gl}"
else
  reg "HKCU\\Software\\Wine\\Direct3D" "renderer" "REG_SZ" "gl"
fi
reg "HKCU\\Software\\Wine\\Direct3D" "OffscreenRenderingMode" "REG_SZ" "backbuffer"
# wined3d reads "csmt" only as a DWORD: the REG_SZ "0" written here until
# 22 Sep 2026 was silently ignored and the command-stream thread stayed on.
# Wine's ddraw makes every Blt synchronous and every Lock is a map, so each one
# was a round trip to that thread -- in a match the render thread spent about a
# quarter of every frame waiting on it (ee-prof samples of wined3d_device_
# context_blt / _map).  Off, the same work runs on the calling thread.
# EE_WINED3D_CSMT=1 turns the thread back on.
reg "HKCU\\Software\\Wine\\Direct3D" "csmt" "REG_DWORD" "${EE_WINED3D_CSMT:-0}"
reg "HKCU\\Software\\Wine\\Mac Driver" "RetinaMode" "REG_SZ" "n"
# DirectDraw's exclusive mode makes the game's window topmost, and by default
# winemac.drv keeps topmost windows floating above every other app even while
# Wine is in the background.  Once ee-ddraw stopped the game minimizing itself
# on Cmd-Tab (22 Sep 2026), that left the game covering whatever you switched
# to.  "none": behind another app, the game's window drops behind its windows.
reg "HKCU\\Software\\Wine\\Mac Driver" "WindowsFloatWhenInactive" "REG_SZ" "none"
reg "HKCU\\Control Panel\\Desktop" "LogPixels" "REG_DWORD" "96"
# EE_EMULATE_MODESET=1 (exported by configure_fullscreen in full screen unless
# the player set it to 0): Wine emulates the game's
# display modes itself and scales the full-screen window to the display, which
# only happens outside a virtual desktop -- inside one, a mode change resizes
# the desktop and the Mac window follows it 1:1.
# Prefix-wide, not per-app: the mode list is built by whichever Wine process
# enumerates the display first (explorer.exe), and without the flag there it is
# the Mac's own scaled resolutions -- no 640x480 or 1024x768, so the game's
# intro and menu mode switches fail.  (The key is named "X11 Driver" but win32u
# reads it for every driver.)
if [[ "${EE_EMULATE_MODESET:-0}" == "1" ]]; then
  VIRTUAL_DESKTOP=0
  reg "HKCU\\Software\\Wine\\X11 Driver" "EmulateModeset" "REG_SZ" "y"
  log "Display modes emulated by Wine (no virtual desktop)"
else
  "$wine_path" reg delete "HKCU\\Software\\Wine\\X11 Driver" /v EmulateModeset /f >/dev/null 2>&1 || true
fi
"$wine_path" reg delete "HKCU\\Software\\Wine\\AppDefaults\\Empire Earth.exe\\X11 Driver" /v EmulateModeset /f >/dev/null 2>&1 || true
if [[ "${VIRTUAL_DESKTOP:-0}" == "1" ]]; then
  # Per-application virtual desktop: every Wine window composites into one Mac
  # NSWindow and GetSystemMetrics reports the desktop size, not the Mac display.
  reg "HKCU\\Software\\Wine\\Explorer\\Desktops" "EmpireEarth" "REG_SZ" "${VIRTUAL_DESKTOP_SIZE:-1440x933}"
  reg "HKCU\\Software\\Wine\\AppDefaults\\Empire Earth.exe\\Explorer" "Desktop" "REG_SZ" "EmpireEarth"
  # Wine actually starts the game as a bare `explorer.exe /desktop` -- no name,
  # no size -- so it looks up Desktops\Default, finds nothing, and falls back to
  # the whole Mac display.  That is why the game came up full-screen and
  # stretched (4:3 content blown into a 1512x982 window) with no way out.
  # Writing Default as well makes the fallback land on the right size.
  reg "HKCU\\Software\\Wine\\Explorer\\Desktops" "Default" "REG_SZ" "${VIRTUAL_DESKTOP_SIZE:-1440x933}"
  reg "HKCU\\Software\\Wine\\Explorer" "Desktop" "REG_SZ" "EmpireEarth"
  log "Virtual desktop EmpireEarth ${VIRTUAL_DESKTOP_SIZE:-1440x933} enabled for Empire Earth.exe"
else
  "$wine_path" reg delete "HKCU\\Software\\Wine\\AppDefaults\\Empire Earth.exe\\Explorer" /v Desktop /f >/dev/null 2>&1 || true
  # The block above also sets the prefix-wide Explorer\Desktop; leaving it
  # behind kept every later run in the virtual desktop regardless.
  "$wine_path" reg delete "HKCU\\Software\\Wine\\Explorer" /v Desktop /f >/dev/null 2>&1 || true
fi

reg "HKCU\\Software\\SSSI\\Empire Earth" "Music Enabled" "REG_DWORD" "0"
reg "HKCU\\Software\\SSSI\\Empire Earth" "Game Bit Depth" "REG_DWORD" "32"
reg "HKCU\\Software\\SSSI\\Empire Earth" "Texture Bit Depth" "REG_DWORD" "32"
reg "HKCU\\Software\\SSSI\\Empire Earth" "Wait for VSync" "REG_DWORD" "0"
reg "HKCU\\Software\\SSSI\\Empire Earth" "Game Window Width" "REG_DWORD" "${EE_GAME_WIDTH:-1440}"
reg "HKCU\\Software\\SSSI\\Empire Earth" "Game Window Height" "REG_DWORD" "${EE_GAME_HEIGHT:-900}"
reg "HKCU\\Software\\SSSI\\Empire Earth" "Rasterizer Name" "REG_SZ" "Direct3D Hardware TnL"
reg "HKCU\\Software\\SSSI\\Empire Earth" "UseCandidateWindow" "REG_DWORD" "0"
reg "HKCU\\Software\\Mad Doc Software\\EE-AOC" "Music Enabled" "REG_DWORD" "0"
reg "HKCU\\Software\\Mad Doc Software\\EE-AOC" "Wait for VSync" "REG_DWORD" "0"
reg "HKCU\\Software\\Mad Doc Software\\EE-AOC" "Rasterizer Name" "REG_SZ" "Direct3D Hardware TnL"

if [[ -n "${GAME_DIR:-}" ]]; then
  skip_movies "$GAME_DIR"
  patch_dxcfg "$GAME_DIR"
  patch_wonlobby "$GAME_DIR"
fi
if [[ -n "${AOC_EXE:-}" && -f "$AOC_EXE" ]]; then
  aoc_dir="$(cd "$(dirname "$AOC_EXE")" && pwd)"
  skip_movies "$aoc_dir"
  patch_dxcfg "$aoc_dir"
  patch_wonlobby "$aoc_dir"
fi

gog_root="$PREFIX/drive_c/GOG Games/Empire Earth Gold Edition"
mkdir -p "$gog_root"
if [[ -n "${GAME_DIR:-}" && -d "$GAME_DIR" ]]; then
  dest="$gog_root/Empire Earth"
  if [[ ! -e "$dest" ]]; then
    ln -s "$GAME_DIR" "$dest"
    log "Linked game into $dest"
  fi
  BASE_EXE="$dest/Empire Earth.exe"
  GAME_DIR="$dest"
fi
if [[ -n "${AOC_EXE:-}" && -f "$AOC_EXE" ]]; then
  aoc_src="$(cd "$(dirname "$AOC_EXE")" && pwd)"
  dest="$gog_root/Empire Earth - The Art of Conquest"
  if [[ ! -e "$dest" ]]; then
    ln -s "$aoc_src" "$dest"
    log "Linked AoC into $dest"
  fi
  AOC_EXE="$dest/EE-AOC.exe"
fi
save_config_preserving_prefs
install_dxvk
if [[ -n "${GAME_DIR:-}" && -d "$GAME_DIR" ]]; then
  install_graphics "$GAME_DIR"
  install_version_shim "$GAME_DIR"
fi
if [[ -n "${AOC_EXE:-}" && -f "$AOC_EXE" ]]; then
  install_graphics "$(cd "$(dirname "$AOC_EXE")" && pwd)"
fi
log "Launch patches applied"
