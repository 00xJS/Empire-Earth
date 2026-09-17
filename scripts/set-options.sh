#!/usr/bin/env bash
# Persist launcher options and apply them to the Wine registry.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"

load_config

apply_only=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --music)
      MUSIC_ENABLED="${2:-0}"
      shift 2
      ;;
    --virtual-desktop)
      # Accept the words as well as the digits -- "--virtual-desktop on" used to
      # silently store the literal string and evaluate as off.
      case "${2:-0}" in
        on|yes|true|1) VIRTUAL_DESKTOP="1" ;;
        off|no|false|0|"") VIRTUAL_DESKTOP="0" ;;
        *) die "Expected on/off after --virtual-desktop, got '${2:-}'" ;;
      esac
      shift 2
      ;;
    --virtual-desktop-size)
      VIRTUAL_DESKTOP_SIZE="${2:-1024x801}"
      shift 2
      ;;
    --graphics)
      case "${2:-}" in
        dgvoodoo|gog-d3d9|dgvoodoo-wined3d|dxmt|d3dmetal|d7vk)
          EE_GRAPHICS="$2"
          ;;
        *)
          die "Unknown graphics stack '$2' (dgvoodoo, gog-d3d9, dgvoodoo-wined3d, dxmt, d3dmetal, d7vk)"
          ;;
      esac
      shift 2
      ;;
    --apply-registry)
      apply_only=1
      shift
      ;;
    *)
      die "Unknown option $1"
      ;;
  esac
done

if [[ "$MUSIC_ENABLED" == "true" ]]; then MUSIC_ENABLED=1; fi
if [[ "$MUSIC_ENABLED" == "false" ]]; then MUSIC_ENABLED=0; fi
if [[ "$VIRTUAL_DESKTOP" == "true" ]]; then VIRTUAL_DESKTOP=1; fi
if [[ "$VIRTUAL_DESKTOP" == "false" ]]; then VIRTUAL_DESKTOP=0; fi

if [[ "$apply_only" -eq 0 ]]; then
  save_config
fi

wine_path="$(find_wine || true)"
if [[ -z "$wine_path" || ! -d "$PREFIX" ]]; then
  exit 0
fi
wine_env "$wine_path"

music_dword=0
if [[ "$MUSIC_ENABLED" == "1" ]]; then
  music_dword=1
fi

"$wine_path" reg add "HKCU\\Software\\SSSI\\Empire Earth" /v "Music Enabled" /t REG_DWORD /d "$music_dword" /f >/dev/null 2>&1 || true
# Art of Conquest uses a sibling key in some installs.
"$wine_path" reg add "HKCU\\Software\\SSSI\\Empire Earth Gold" /v "Music Enabled" /t REG_DWORD /d "$music_dword" /f >/dev/null 2>&1 || true
