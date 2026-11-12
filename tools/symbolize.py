#!/usr/bin/env python3
"""Adds function names and source lines to the addresses in a log.txt from
the 3DS build.

Usage: symbolize.py LOG [ELF]     (ELF defaults to build/lom3ds.elf)

The log must come from the same build as the ELF: its first line carries
the build stamp, which is checked against the one in the ELF.
"""
import os
import pathlib
import re
import subprocess
import sys

PORT = pathlib.Path(__file__).resolve().parents[1]
devkitarm = os.environ.get("DEVKITARM", str(PORT.parent / "toolchain" / "devkitpro" / "devkitARM"))
log = pathlib.Path(sys.argv[1]).read_text(errors="replace")
elf = sys.argv[2] if len(sys.argv) > 2 else str(PORT / "build" / "lom3ds.elf")

# __DATE__ __TIME__, e.g. "Nov 11 2026 04:59:00" (the day is space-padded).
stamp = re.search(r"build (\w{3} [ \d]\d \d{4} \d\d:\d\d:\d\d)", log)
if not stamp:
    print("warning: the log has no build stamp; cannot check it against the ELF\n")
elif stamp.group(1).encode() not in pathlib.Path(elf).read_bytes():
    print(f"warning: the log is from build {stamp.group(1)}, which is not {elf}; names may be wrong\n")

for line in log.split("\n"):
    print(line)
    if not line.startswith(("crash", "  stack", "  pc")):
        continue
    for addr in re.findall(r"\b(00[1-9a-f][0-9a-f]{5})\b", line):
        out = subprocess.run([f"{devkitarm}/bin/arm-none-eabi-addr2line", "-f", "-p", "-e", elf, addr],
                             capture_output=True, text=True).stdout.strip()
        if out and not out.startswith("??"):
            print(f"      {addr}  {out}")
