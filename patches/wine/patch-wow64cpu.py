#!/usr/bin/env python3
"""Make Wine 11.0's wow64cpu.dll survive Rosetta 2 missing a CPU mode switch.

Every syscall and unix call a 32-bit Windows program makes under Wine's new
WoW64 crosses from 32-bit to 64-bit code through a thunk (`jmp far [ptr]`,
selector cs64) into a landing pad, and returns with `ljmp *(%r14)` (selector
cs32).  Under Rosetta 2, a thread crossing while another thread makes Rosetta
discard translated code (a DLL unloaded, a code page re-protected, code
rewritten) can arrive in the wrong mode:

  * 32->64: the 64-bit landing pad runs as 32-bit code, and its first
    RIP-relative load reads its own displacement as an absolute address --
    the whole "page fault on read access to 00004DC9 / 00004ECD" signature.
  * 64->32: 32-bit code runs as 64-bit code until a `ret` pops 8 bytes and
    jumps to <garbage>:<eip>.

CodeWeavers fixed this in CrossOver as "CW HACK 20760" (enter with `lcall`,
leave with `lretq`); Gcenx's wine-stable 11.0_1 build dropped that patch.
This restores it by binary patch, and on top of it checks the mode after
every crossing and redoes any crossing Rosetta still gets wrong.  The new code
is wow64cpu-rosetta.S (next to this file), assembled here with the MinGW
toolchain the project already needs.

Redo counters live in .data: RVA 0x2ff0 syscall entries, 0x2ff4 unix-call
entries, 0x2ff8 returns (the DLL is mapped at 0x7bf20000 in Wine processes).

The patch refuses any file but the exact Wine Stable 11.0 build it was
written against, and verifies every byte it replaces.

Usage: patch-wow64cpu.py <in.dll> <out.dll>
"""
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile

ORIGINAL_SHA1 = "84d25783974000f5bd5832d02db524ff970eeae0"   # Gcenx wine-stable-11.0_1

NEW_CODE = 0x1A00          # zero padding at the end of .text's raw data (file offset == RVA)
CALL_SITE = 0x1552         # BTCpuProcessInit: lea 0x34(%rsp),%rax, just before NtProtectVirtualMemory
THUNK_SIZE = 0x1413        # BTCpuProcessInit: movq $0x18,0x40(%rsp)  (size passed to NtProtect)
GET_BOP_CODE = 0x13B0      # BTCpuGetBopCode:        lea thunk+0x0(%rip),%rax ; ret
GET_UNIX_OPCODE = 0x1930   # __wine_get_unix_opcode: lea thunk+0xc(%rip),%rax ; ret
FAST_RETURNS = (0x1191, 0x127A)   # mov 0xb8(%r13),%edx ... xchg %r14,%rsp ; ljmp *(%r14)
THUNK_SYSCALL, THUNK_UNIXCALL = 0x7020, 0x7060
COUNTERS = 0x2FF0
TEXT_HDR, DATA_HDR = 0x188, 0x1B0   # section headers (VirtualSize at +8)

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL_PREFIX = os.environ.get("MINGW64_PREFIX", "x86_64-w64-mingw32-")


def rel32(src_next, dst):
    return struct.pack("<i", dst - src_next)


def expect(buf, off, want, what):
    got = bytes(buf[off:off + len(want)])
    if got != want:
        sys.exit(f"patch-wow64cpu: unexpected bytes at 0x{off:x} ({what}): {got.hex()} != {want.hex()}")


def tool(name):
    path = shutil.which(TOOL_PREFIX + name)
    if not path:
        sys.exit(f"patch-wow64cpu: {TOOL_PREFIX}{name} not found (brew install mingw-w64)")
    return path


def assemble():
    """Return (code bytes, {symbol: offset}) for wow64cpu-rosetta.S."""
    with tempfile.TemporaryDirectory() as tmp:
        obj, raw = os.path.join(tmp, "r.o"), os.path.join(tmp, "r.bin")
        subprocess.run([tool("as"), "-o", obj, os.path.join(HERE, "wow64cpu-rosetta.S")], check=True)
        relocs = subprocess.run([tool("objdump"), "-r", obj], check=True, capture_output=True, text=True).stdout
        if "R_X86_64" in relocs or "IMAGE_REL" in relocs:
            sys.exit("patch-wow64cpu: wow64cpu-rosetta.S must assemble without relocations")
        subprocess.run([tool("objcopy"), "-O", "binary", "-j", ".text", obj, raw], check=True)
        code = open(raw, "rb").read()
        syms = {}
        for line in subprocess.run([tool("nm"), obj], check=True, capture_output=True, text=True).stdout.splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[1] in "tT":
                syms[parts[2]] = int(parts[0], 16)
    return code, syms


def main():
    src, dst = sys.argv[1], sys.argv[2]
    data = bytearray(open(src, "rb").read())
    sha1 = hashlib.sha1(data).hexdigest()
    if sha1 != ORIGINAL_SHA1:
        sys.exit(f"patch-wow64cpu: {src} is not the Wine Stable 11.0 wow64cpu.dll this patch targets (sha1 {sha1})")

    code, syms = assemble()
    write_thunks = NEW_CODE + syms["write_thunks"]
    newret = NEW_CODE + syms["newret"]
    if NEW_CODE + len(code) > 0x2000:
        sys.exit("patch-wow64cpu: new code does not fit in .text padding")
    if any(data[NEW_CODE:0x2000]) or any(data[COUNTERS:COUNTERS + 12]):
        sys.exit("patch-wow64cpu: the padding this patch uses is not empty")

    expect(data, CALL_SITE, bytes.fromhex("488d442434"), "lea 0x34(%rsp),%rax before NtProtect")
    expect(data, THUNK_SIZE, bytes.fromhex("48c7442440" "18000000"), "thunk size 0x18")
    expect(data, GET_BOP_CODE, bytes.fromhex("488d05") + rel32(GET_BOP_CODE + 7, 0x7000) + b"\xc3", "BTCpuGetBopCode")
    expect(data, GET_UNIX_OPCODE, bytes.fromhex("488d05") + rel32(GET_UNIX_OPCODE + 7, 0x700C) + b"\xc3",
           "__wine_get_unix_opcode")
    fast_return = bytes.fromhex("418b95b8000000" "891424" "418b95bc000000" "89542404"
                                "458bb5c4000000" "4c87f4" "41ff2e")
    for site in FAST_RETURNS:
        expect(data, site, fast_return, f"fast return at 0x{site:x}")

    data[NEW_CODE:NEW_CODE + len(code)] = code
    data[CALL_SITE:CALL_SITE + 5] = b"\xe8" + rel32(CALL_SITE + 5, write_thunks)
    data[THUNK_SIZE + 5:THUNK_SIZE + 9] = struct.pack("<I", 0x90)      # protect the new thunks too
    data[GET_BOP_CODE + 3:GET_BOP_CODE + 7] = rel32(GET_BOP_CODE + 7, THUNK_SYSCALL)
    data[GET_UNIX_OPCODE + 3:GET_UNIX_OPCODE + 7] = rel32(GET_UNIX_OPCODE + 7, THUNK_UNIXCALL)
    for site in FAST_RETURNS:
        data[site:site + 5] = b"\xe9" + rel32(site + 5, newret)
    # Map the padding the new code and the counters now use.
    struct.pack_into("<I", data, TEXT_HDR + 8, 0x1000)
    struct.pack_into("<I", data, DATA_HDR + 8, 0x1000)

    open(dst, "wb").write(data)
    print(f"patch-wow64cpu: wrote {dst} ({len(code)} bytes new code; write_thunks=0x{write_thunks:x} "
          f"newret=0x{newret:x}; sha1 {hashlib.sha1(data).hexdigest()})")


if __name__ == "__main__":
    main()
