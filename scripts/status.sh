#!/usr/bin/env bash
# Print launcher status as JSON for the Swift app.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"

load_config
wine_path="$(find_wine || true)"
winetricks_path="$(find_winetricks || true)"
prefix_ok=0
if [[ -f "$READY_STAMP" ]] && directmusic_present; then
  prefix_ok=1
fi
dm=0
if directmusic_present; then
  dm=1
fi

python3 - "$wine_path" "$winetricks_path" "$PREFIX" "$prefix_ok" "$dm" \
  "${GAME_DIR:-}" "${BASE_EXE:-}" "${AOC_EXE:-}" \
  "${MUSIC_ENABLED:-0}" "${VIRTUAL_DESKTOP:-0}" "${VIRTUAL_DESKTOP_SIZE:-1920x1080}" \
  "${EE_GRAPHICS:-dgvoodoo}" <<'PY'
import json, os, sys

def flag(value):
    return str(value) in {"1", "true", "True"}

wine, winetricks, prefix, prefix_ok, dm = sys.argv[1:6]
game_dir, base_exe, aoc_exe = sys.argv[6:9]
music, vd, vd_size = sys.argv[9:12]
graphics = sys.argv[12] if len(sys.argv) > 12 else "dgvoodoo"
errors = []
warnings = []
if not wine:
    errors.append("Wine is not installed. Click Install Wine — it is free and no extra purchase is required.")
if not winetricks:
    warnings.append("winetricks is not installed yet. Set Up Prefix will install it with Homebrew.")
if prefix_ok != "1":
    errors.append("The Wine prefix is not ready. Click Set Up Prefix.")
if not base_exe or not os.path.isfile(base_exe):
    errors.append(
        "No Empire Earth.exe in that folder. Select your GOG Empire Earth Gold folder "
        "(it contains Empire Earth.exe), or choose the GOG setup .exe to install into the Wine prefix."
    )
if aoc_exe and not os.path.isfile(aoc_exe):
    warnings.append(f"Saved Art of Conquest path is missing: {aoc_exe}")
    aoc_exe = ""
if graphics == "d3dmetal":
    warnings.append("D3DMetal/GPTK is 64-bit only on this Mac. Empire Earth is 32-bit; Play will explain and stop.")
payload = {
    "wine_path": wine or None,
    "wine_ok": bool(wine),
    "winetricks_path": winetricks or None,
    "prefix": prefix,
    "prefix_ok": prefix_ok == "1",
    "direct_music": dm == "1",
    "game_dir": game_dir or None,
    "base_exe": base_exe if base_exe and os.path.isfile(base_exe) else None,
    "aoc_exe": aoc_exe or None,
    "music_enabled": flag(music),
    "virtual_desktop": flag(vd),
    "virtual_desktop_size": vd_size or "1920x1080",
    "graphics_stack": graphics or "dgvoodoo",
    "can_play": bool(wine and prefix_ok == "1" and base_exe and os.path.isfile(base_exe)),
    "can_play_aoc": bool(wine and prefix_ok == "1" and aoc_exe and os.path.isfile(aoc_exe) and base_exe and os.path.isfile(base_exe)),
    "errors": errors,
    "warnings": warnings,
}
print(json.dumps(payload))
PY
