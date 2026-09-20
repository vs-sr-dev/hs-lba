#!/usr/bin/env python3
"""
mkcd.py — build the HS-LBA data image.

Layout is deliberately NOT ISO9660: the console's slow CD mech makes seeks,
not throughput, the cost that matters, so data is a flat archive with a fixed
index at LBA 0 and every file stored contiguously. One seek per file, then
pure sequential streaming.

(The shipping disc will still carry a minimal ISO9660 filesystem so the BIOS
can find HYPER.EXE; this archive simply lives alongside it.)

Index at LBA 0:
    magic   char[8]  "HSLBA1\0\0"
    count   u32
    pad     u32
    entries count * { name char[16]; lba u32; size u32; sum u32 }

`sum` is a simple additive-rotate checksum over the file bytes, so the target
can verify an end-to-end read without a host in the loop.
"""

import struct
import sys
from pathlib import Path

SECTOR = 2048


def checksum(data: bytes) -> int:
    """FNV-1a. Deliberately rotate-free: GCC compiles any rotate idiom to the
    score `rori`/`roli` family, and MAME's score core leaves every rotate
    unemulated (fatal error). Multiply is fine — the CPU has a hardware
    multiplier."""
    s = 2166136261
    for b in data:
        s = (s ^ b) & 0xFFFFFFFF
        s = (s * 16777619) & 0xFFFFFFFF
    return s


def sectors_for(n: int) -> int:
    return (n + SECTOR - 1) // SECTOR


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: mkcd.py <data-dir> <out.iso> [files...]")
        return 2

    data_dir = Path(sys.argv[1])
    out_path = Path(sys.argv[2])
    names = sys.argv[3:]

    files = ([data_dir / n for n in names] if names
             else sorted(data_dir.glob("*.HQR")))
    files = [f for f in files if f.is_file()]
    if not files:
        print(f"no input files in {data_dir}")
        return 1

    index_sectors = sectors_for(16 + len(files) * 28)
    lba = index_sectors

    entries = []
    for f in files:
        payload = f.read_bytes()
        entries.append((f.name.upper(), lba, len(payload), checksum(payload), payload))
        lba += sectors_for(len(payload))

    index = bytearray(struct.pack("<8sII", b"HSLBA1", len(entries), 0))
    for name, elba, size, csum, _ in entries:
        index += struct.pack("<16sIII", name.encode("ascii")[:15], elba, size, csum)
    index += b"\0" * (index_sectors * SECTOR - len(index))

    with out_path.open("wb") as out:
        out.write(index)
        for name, elba, size, csum, payload in entries:
            assert out.tell() == elba * SECTOR, f"{name} misplaced"
            out.write(payload)
            pad = sectors_for(size) * SECTOR - size
            out.write(b"\0" * pad)

    total = out_path.stat().st_size
    print(f"{out_path}  {total:,} bytes  ({total // SECTOR} sectors)")
    print(f"{'file':<16}{'lba':>8}{'size':>12}{'sectors':>9}  checksum")
    for name, elba, size, csum, _ in entries:
        print(f"{name:<16}{elba:>8}{size:>12,}{sectors_for(size):>9}  {csum:08X}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
