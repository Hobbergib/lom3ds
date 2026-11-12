#!/usr/bin/env python3
"""Builds small synthetic disc images for the host tests.

They hold no game data: a SYSTEM.CNF naming a boot executable, a made-up
executable, and one file in a subdirectory. Writes DIR/raw/test.bin
(2352-byte Mode 2 sectors), DIR/cooked/test.iso (2048-byte sectors),
DIR/junk/notadisc.bin and DIR/expected.txt with the executable's SHA-1.
"""
import hashlib
import os
import struct
import sys

SECTOR = 2048


def both(n):
    return struct.pack("<I", n) + struct.pack(">I", n)


def record(name: bytes, lba: int, size: int, is_dir: bool) -> bytes:
    length = 33 + len(name)
    length += length & 1
    rec = bytes([length, 0]) + both(lba) + both(size) + bytes(7) + bytes([2 if is_dir else 0, 0, 0])
    rec += struct.pack("<H", 1) + struct.pack(">H", 1) + bytes([len(name)]) + name
    return rec.ljust(length, b"\0")


def pad(data: bytes) -> bytes:
    return data.ljust(-(-len(data) // SECTOR) * SECTOR, b"\0")


def build() -> tuple[bytes, bytes]:
    exe = bytes((i * 7 + (i >> 8)) & 0xFF for i in range(100_000))  # spans several read chunks
    cnf = b"BOOT = cdrom:\\SLUS_010.13;1\r\nTCB = 4\r\nEVENT = 16\r\nSTACK = 801FFFF0\r\n"
    field = b"field overlay stand-in" * 200

    root_lba, bin_lba, cnf_lba = 18, 19, 20
    exe_lba = cnf_lba + 1
    field_lba = exe_lba + len(pad(exe)) // SECTOR

    root = record(b"\0", root_lba, SECTOR, True) + record(b"\1", root_lba, SECTOR, True)
    root += record(b"BIN", bin_lba, SECTOR, True)
    root += record(b"SLUS_010.13;1", exe_lba, len(exe), False)
    root += record(b"SYSTEM.CNF;1", cnf_lba, len(cnf), False)
    bindir = record(b"\0", bin_lba, SECTOR, True) + record(b"\1", root_lba, SECTOR, True)
    bindir += record(b"FIELD.BIN;1", field_lba, len(field), False)

    pvd = bytearray(SECTOR)
    pvd[0:6] = b"\1CD001"
    pvd[6] = 1
    pvd[156:190] = record(b"\0", root_lba, SECTOR, True)
    term = b"\xffCD001\1".ljust(SECTOR, b"\0")

    image = bytes(16 * SECTOR) + bytes(pvd) + term + pad(root) + pad(bindir) + pad(cnf) + pad(exe) + pad(field)
    return image, exe


def to_raw(image: bytes) -> bytes:
    sync = b"\0" + b"\xff" * 10 + b"\0"
    out = bytearray()
    for i in range(len(image) // SECTOR):
        header = sync + bytes([0, 2, i % 75, 2]) + bytes([0, 0, 8, 0]) * 2
        out += header + image[i * SECTOR:(i + 1) * SECTOR] + bytes(280)
    return bytes(out)


def main():
    out = sys.argv[1]
    image, exe = build()
    for sub in ("raw", "cooked", "junk", "empty"):
        os.makedirs(os.path.join(out, sub), exist_ok=True)
    open(os.path.join(out, "raw", "test.bin"), "wb").write(to_raw(image))
    open(os.path.join(out, "cooked", "test.iso"), "wb").write(image)
    open(os.path.join(out, "junk", "notadisc.bin"), "wb").write(b"not a disc image" * 5000)
    open(os.path.join(out, "expected.txt"), "w").write(hashlib.sha1(exe).hexdigest() + "\n")


if __name__ == "__main__":
    main()
