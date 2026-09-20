#!/usr/bin/env python3
"""Make a blank RFID card image for MAME.

MAME's hyperscan_card device declares is_creatable() == false, so it will not
invent one: -memc needs a file that already exists and is exactly 120 bytes.

The layout is a Type 1 (Topaz/Jewel) tag:

    0x00-0x06   UID           read-only on real hardware
    0x07        reserved
    0x08-0x67   user data     96 bytes, all a save is allowed to touch
    0x68-0x6f   reserved
    0x70-0x71   lock bytes    one-way; setting them bricks the card
    0x72-0x77   OTP

MAME lets a program write every one of those and real silicon does not, which
is why src/card.c addresses nothing outside 0x08-0x67 and why this file leaves
the rest as a real blank card would have it.

    python tools/mkcard.py build/card_blank.bin
    python tools/mkcard.py build/card_blank.bin --uid E1234567

This alone does NOT reset a card MAME has already written. The device persists
through battery_save(), into <mame>/nvram/<machine>/<basename>.nv, and
call_load() lets that file overwrite whatever the .bin holds. To really start
from a blank card, delete the .nv too. tools/cardinfo.py reads the .nv.
"""

import argparse
import sys

CARD_BYTES = 120
USER_BASE = 0x08
USER_SIZE = 96


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("output")
    ap.add_argument("--uid", default="E1234567",
                    help="four UID bytes as hex (default E1234567)")
    args = ap.parse_args()

    try:
        uid = bytes.fromhex(args.uid)
    except ValueError:
        print(f"mkcard: --uid must be hex, got {args.uid!r}", file=sys.stderr)
        return 1

    if len(uid) != 4:
        print(f"mkcard: --uid must be 4 bytes, got {len(uid)}", file=sys.stderr)
        return 1

    card = bytearray(CARD_BYTES)

    # RID and every addressed command carry UID bytes 0-3; MAME reads them from
    # here and ignores what a command sends back, but a real tag does not.
    card[0:4] = uid
    card[4:7] = b"\x00\x00\x00"

    with open(args.output, "wb") as f:
        f.write(card)

    print(f"{args.output}: {CARD_BYTES} bytes, UID {uid.hex().upper()}, "
          f"user area {USER_BASE:#04x}..{USER_BASE + USER_SIZE - 1:#04x} blank")
    return 0


if __name__ == "__main__":
    sys.exit(main())
