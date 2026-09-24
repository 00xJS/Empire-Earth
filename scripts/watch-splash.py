#!/usr/bin/env python3
"""Stop Empire Earth if the opening banner is still the visible window after 60s.

An 800x600 DXVK swapchain is not enough: that can present off-screen while the
640x260 GDI splash stays up. Play must return within one minute.
"""
from __future__ import annotations

import os
import re
import subprocess
import sys
import time
from pathlib import Path


MAX_TIMEOUT = 1200


# The game's command line starts with its Windows path.  A bare substring
# match also hits our own `wine reg add ...\AppDefaults\Empire Earth.exe\...`.
GAME_ARGS = re.compile(r"^[A-Za-z]:\\.*\\(Empire Earth|EE-AOC)\.exe")
# Written to the launch log by Wine when the game crashes.
CRASH_SIGNS = ("Unhandled page fault", "Unhandled exception", "starting debugger", "Program Error")


def game_running() -> bool:
    """True while a live game process exists (see game_pids in lib.sh).

    A Rosetta process SIGKILLed after a crash can linger in the process table,
    stuck mid-exit with ~8 KB resident; counting it as alive is what made
    every retry after a crash wait out the whole timeout.
    """
    out = subprocess.check_output(["ps", "-axo", "rss=,args="], text=True, errors="replace")
    for line in out.splitlines():
        parts = line.strip().split(None, 1)
        if len(parts) == 2 and parts[0].isdigit() and int(parts[0]) > 1024 and GAME_ARGS.match(parts[1]):
            return True
    return False


# ee-ddraw.log (the d7vk stack's proxy log), exported by launch.sh.  Until the
# game has drawn its first frame this log grows steadily -- plugin scan, device
# creation, texture uploads -- so if it stops changing for STALL_SECONDS the
# game is hung, not loading.  Seen on 22 Sep 2026 in the plugin scan and inside
# CreateDevice, a few launches in ten; nothing has been played yet, so killing
# it and starting over is safe.  After the first frame a quiet log means nothing:
# the menu idles while the game is not the active app.
DDRAW_LOG = os.environ.get("EE_DDRAW_LOG", "")
STALL_SECONDS = int(os.environ.get("EE_STALL_SECONDS", "60"))
# A start counts once the game has begun this many frames.  One is not enough:
# Art of Conquest at its 16-bit defaults drew a single frame and hung on a black
# screen, and was reported as running (23 Sep 2026).  The proxy logs the first
# 12 BeginScene calls, so the count is reliable up to there.
FRAMES_TO_START = 10


def ddraw_state() -> tuple[bool, tuple[int, str]]:
    """(first frames drawn?, a signature that changes only while the game makes progress).

    The proxy's own heartbeat ("progress:" every 2 s, "window[tick]") keeps the
    file growing even when the game is stuck, so those lines are left out; the
    counters inside the last progress line stay identical once nothing happens.
    """
    if not DDRAW_LOG:
        return True, (0, "")
    try:
        text = Path(DDRAW_LOG).read_text(errors="replace")
    except OSError:
        return False, (0, "")
    other, progress = 0, ""
    for line in text.splitlines():
        if "progress:" in line:
            progress = line.split("progress:", 1)[1]
        elif "window[tick]" not in line:
            other += 1
    return text.count("BeginScene ENTER") >= FRAMES_TO_START, (other, progress)


def log_shows_crash(log_path: str) -> bool:
    try:
        text = Path(log_path).read_text(errors="replace")
    except OSError:
        return False
    return any(sign in text for sign in CRASH_SIGNS)


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
    last_sig = None
    last_change = time.time()
    while time.time() < deadline:
        if log_shows_crash(log_path):
            append(log_path, "Game process crashed before leaving the opening screen.")
            stop_wine(wineserver, prefix)
            return 1
        if game_running():
            started = True
            if not captured and time.time() - started_at >= 6:
                captured = True
                capture_debug(log_path)
            rendered, sig = ddraw_state()
            if sig != last_sig:
                last_sig, last_change = sig, time.time()
            elif not rendered and time.time() - last_change >= STALL_SECONDS:
                append(log_path, f"Game process stalled before its first frames (no progress for {STALL_SECONDS}s).")
                stop_wine(wineserver, prefix)
                return 1
            # Success is the first frame actually drawn, not just the splash
            # window going away: a window can be up while the game is still hung
            # inside CreateDevice.
            if rendered and past_splash(probe):
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
    if past_splash(probe) and game_running() and ddraw_state()[0]:
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
