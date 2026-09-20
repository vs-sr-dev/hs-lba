#!/usr/bin/env python3
"""Decode a saved game off an RFID card image.

    python tools/cardinfo.py nvram/hyprscan/card_blank.nv

WHERE THE CARD ACTUALLY IS. Not where you put it. MAME's memcard device loads
the file named by -memc but persists through battery_save(), which writes to

    <mame>/nvram/<machine>/<image basename>.nv

so the .bin handed to -memc stays exactly as mkcard.py wrote it however many
saves the game performs, and the .nv is the card. Worse, call_load() reads the
.bin and then lets battery_load() overwrite it from the .nv, so a "blank" card
handed to a later run is not blank — delete the .nv to really start over.

The layout is src/save.c's; keep the offsets below in step with the C_* defines
there. Everything is little-endian, as the console is.
"""

import argparse
import sys

USER_BASE, USER_SIZE = 8, 96
MAGIC, FORMAT = 0x4C, 1

N_FLAGS, N_HOLO, N_INV = 255, 150, 28

C_MAGIC, C_VERSION, C_SUM = 0, 1, 2
C_FLAGS, C_HOLO, C_INV = 3, 35, 54
C_NEXC, C_EXC = 80, 81
MAX_EXC = 7

SCALARS = [
    ("NumCube", 58, 1), ("Chapitre", 59, 1), ("Comportement", 60, 1),
    ("LifePoint", 61, 1), ("NbGoldPieces", 62, 2), ("MagicLevel", 64, 1),
    ("MagicPoint", 65, 1), ("NbCloverBox", 66, 1), ("NbFourLeafClover", 67, 1),
    ("Fuel", 68, 1), ("Weapon", 69, 2), ("SceneStartX", 71, 2),
    ("SceneStartY", 73, 2), ("SceneStartZ", 75, 2), ("Beta", 77, 2),
    ("GenBody", 79, 1),
]


def card_sum(u: bytes) -> int:
    s = 0x5A
    for i in range(3, USER_SIZE):
        s = ((s << 1) | (s >> 7)) & 0xFF
        s ^= u[i]
    return s ^ u[C_VERSION]


def bits_set(u: bytes, base: int, count: int):
    return [i for i in range(count) if (u[base + (i >> 3)] >> (i & 7)) & 1]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    ap.add_argument("--hex", action="store_true", help="dump the 96 raw bytes")
    args = ap.parse_args()

    data = open(args.image, "rb").read()
    if len(data) != 120:
        print(f"cardinfo: {args.image} is {len(data)} bytes, not 120",
              file=sys.stderr)
        return 1

    u = data[USER_BASE:USER_BASE + USER_SIZE]

    print(f"{args.image}")
    print(f"  UID            {data[0:4].hex().upper()}")

    if args.hex:
        for i in range(0, USER_SIZE, 16):
            print(f"  {i:02x}: {u[i:i + 16].hex(' ')}")

    if u[C_MAGIC] != MAGIC:
        print(f"  magic          {u[C_MAGIC]:#04x} - no saved game "
              f"(blank card, another game's card, or a save torn mid-write)")
        return 0

    want = card_sum(u)
    ok = want == u[C_SUM]
    print(f"  magic          {u[C_MAGIC]:#04x} 'L'")
    print(f"  format         {u[C_VERSION]}"
          + ("" if u[C_VERSION] == FORMAT else f"  (this tool knows {FORMAT})"))
    print(f"  checksum       {u[C_SUM]:#04x} " + ("ok" if ok else f"BAD, want {want:#04x}"))

    if not ok:
        print("  the game would refuse this card")

    print()
    for name, off, size in SCALARS:
        v = u[off] if size == 1 else u[off] | (u[off + 1] << 8)
        print(f"  {name:<18} {v}")

    flags = bits_set(u, C_FLAGS, N_FLAGS)
    holo = bits_set(u, C_HOLO, N_HOLO)
    inv = bits_set(u, C_INV, N_INV)

    print()
    print(f"  game flags set     {len(flags)}/{N_FLAGS}  {flags[:24]}"
          + (" ..." if len(flags) > 24 else ""))
    print(f"  holomap positions  {len(holo)}/{N_HOLO}  {holo[:24]}"
          + (" ..." if len(holo) > 24 else ""))
    print(f"  inventory used     {len(inv)}/{N_INV}  {inv}")

    # The exception list is the part worth watching over a playthrough: it is
    # the only thing in the format with a hard limit, and src/save.c refuses a
    # save outright rather than truncate it.
    nexc = u[C_NEXC]
    print()
    print(f"  flag exceptions    {nexc}/{MAX_EXC}"
          + ("  <- FULL, the next non-boolean flag refuses the save"
             if nexc >= MAX_EXC else ""))
    for i in range(min(nexc, MAX_EXC)):
        print(f"    flag {u[C_EXC + i * 2]:>3} = {u[C_EXC + i * 2 + 1]}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
