#!/usr/bin/env python3
"""Stop Empire Earth if the opening banner is still the visible window after 60s.

An 800x600 DXVK swapchain is not enough: that can present off-screen while the
640x260 GDI splash stays up. Play must return within one minute.
"""
from __future__ import annotations

import os
import subprocess
import sys
import time
from pathlib import Path


MAX_TIMEOUT = 1200


def game_running() -> bool:
    out = subprocess.check_output(["ps", "-axo", "comm="], text=True, errors="replace")
    if "Empire Earth.exe" in out or "EE-AOC.exe" in out:
        return True
    # Under a virtual desktop the Unix command name is explorer.exe; the game
    # only shows up in the Windows command line.
    args = subprocess.check_output(["ps", "-axo", "args="], text=True, errors="replace")
    return "Empire Earth.exe" in args or "EE-AOC.exe" in args


def probe_bin() -> Path:
    support = Path.home() / "Library/Application Support/EmpireEarthMac/patches/macos"
    return support / "ee-splash-probe"


def ensure_probe() -> Path | None:
    dest = probe_bin()
    src = Path(__file__).resolve().parent.parent / "patches/macos/ee-splash-probe.c"
    if not src.is_file():
        return dest if dest.is_file() else None
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.is_file() and dest.stat().st_mtime >= src.stat().st_mtime:
        return dest
    log = Path.home() / "Library/Application Support/EmpireEarthMac/logs/ee-splash-probe.build.log"
    result = subprocess.run(
        [
            "clang",
            "-O2",
            "-framework",
            "CoreGraphics",
            "-framework",
            "CoreFoundation",
            "-o",
            str(dest),
            str(src),
        ],
        capture_output=True,
        text=True,
    )
    log.write_text(result.stdout + result.stderr)
    if result.returncode != 0:
        return dest if dest.is_file() else None
    return dest


def past_splash(probe: Path | None) -> bool:
    if probe is None or not probe.is_file():
        return False
    result = subprocess.run([str(probe)], capture_output=True, text=True)
    return result.returncode == 0


def append(log_path: str, line: str) -> None:
    with open(log_path, "a") as handle:
        handle.write(line + "\n")


def stop_wine(wineserver: str, prefix: str) -> None:
    if wineserver and prefix and os.path.isfile(wineserver):
        env = os.environ.copy()
        env["WINEPREFIX"] = prefix
        subprocess.call(
            [wineserver, "-k"],
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )


def capture_debug(log_path: str) -> None:
    shot = Path(log_path).with_suffix(".screen.png")
    subprocess.run(["screencapture", "-x", str(shot)], check=False)
    append(log_path, f"Debug screenshot: {shot}")
    probe = probe_bin()
    if not probe.is_file():
        return
    result = subprocess.run([str(probe)], capture_output=True, text=True)
    append(log_path, (result.stderr or "").strip())
    for line in (result.stderr or "").splitlines():
        if "wine id=" not in line:
            continue
        try:
            wid = line.split("wine id=", 1)[1].split()[0]
            dest = Path(log_path).with_name(Path(log_path).stem + f"-wine-{wid}.png")
            subprocess.run(["screencapture", "-x", "-l" + wid, str(dest)], check=False)
            append(log_path, f"Wine window screenshot: {dest}")
        except (IndexError, ValueError):
            continue


def main() -> int:
    log_path = sys.argv[1]
    raw = int(sys.argv[2] if len(sys.argv) > 2 else os.environ.get("EE_MENU_TIMEOUT", "60"))
    timeout = min(max(raw, 1), MAX_TIMEOUT)
    wineserver = os.environ.get("WINESERVER", "")
    prefix = os.environ.get("WINEPREFIX", "")
    probe = ensure_probe()
    deadline = time.time() + timeout
    started_at = time.time()
    started = False
    captured = False
    while time.time() < deadline:
        if game_running():
            started = True
            if not captured and time.time() - started_at >= 6:
                captured = True
                capture_debug(log_path)
            if past_splash(probe):
                if not captured:
                    capture_debug(log_path)
                append(log_path, "Past the opening screen; leaving the game running.")
                return 0
        elif started:
            append(log_path, "Game process exited before leaving the opening screen.")
            return 1
        elif time.time() - started_at >= 20:
            append(log_path, "Game process did not start within 20s; stopping.")
            stop_wine(wineserver, prefix)
            return 1
        time.sleep(1)
    if past_splash(probe) and game_running():
        append(log_path, "Past the opening screen; leaving the game running.")
        return 0
    append(
        log_path,
        f"Still on the opening screen after {timeout}s; stopping.",
    )
    stop_wine(wineserver, prefix)
    return 1


if __name__ == "__main__":
    sys.exit(main())
