#!/usr/bin/env python3
"""
stage_data.py — copy the game files this port needs out of your own copy of
Little Big Adventure and into data/, where the Makefile's `iso` target picks
them up.

    python tools/stage_data.py "I:/GOG/Little Big Adventure"

or set LBA1_ASSETS to that directory and run it with no argument. The
argument is the install root — the directory that contains Common/ and
CommonClassic/.

Which release matters. This port reads the **CD** data as GOG ships it in
Common/ (640x480 art, LZSS archives, the FLA movies and the VOX voice banks);
TEXT.HQR alone lives in CommonClassic/, and Speedrun/Windows/ is the DOS
floppy-era set that the DS port uses and this one does not.

data/LBA.CFG is ours and is already in the repository — it is a config file
written for this console, not a game asset — so it is left alone.
"""

import os
import shutil
import sys
from pathlib import Path

CORE_HQR = ["ANIM.HQR", "BODY.HQR", "FILE3D.HQR", "INVOBJ.HQR", "LBA_BLL.HQR",
            "LBA_BRK.HQR", "LBA_GRI.HQR", "RESS.HQR", "SAMPLES.HQR",
            "SCENE.HQR", "SPRITES.HQR"]

# One language only: the twelve EN_*.VOX are 32 MB, and five languages would
# push the image past what the flat archive was sized for.
LANG = "EN"


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("LBA1_ASSETS")
    if not root:
        sys.exit(__doc__)
    root = Path(root)
    common, classic = root / "Common", root / "CommonClassic"
    for d in (common, classic, common / "Fla", common / "Vox"):
        if not d.is_dir():
            sys.exit(f"not found: {d} — is {root} the GOG install root?")

    out = Path(__file__).resolve().parent.parent / "data"
    out.mkdir(exist_ok=True)

    plan = [(common / n, out / n) for n in CORE_HQR]
    plan.append((classic / "TEXT.HQR", out / "TEXT.HQR"))
    plan.append((common / "Fla" / "FLASAMP.HQR", out / "FLASAMP.HQR"))
    plan += [(p, out / p.name) for p in sorted(common.glob("Fla/*.FLA"))]
    plan += [(p, out / p.name) for p in sorted(common.glob(f"Vox/{LANG}_*.VOX"))]

    total = 0
    for src, dst in plan:
        if not src.is_file():
            sys.exit(f"missing: {src}")
        if dst.exists() and dst.stat().st_size == src.stat().st_size:
            continue
        shutil.copyfile(src, dst)
        total += src.stat().st_size
        print(f"  {src.name:14s} {src.stat().st_size:>10,d}")

    cfg = out / "LBA.CFG"
    if not cfg.is_file():
        sys.exit("data/LBA.CFG is missing — it is part of the repository")
    if b"\r\n" not in cfg.read_bytes():
        sys.exit("data/LBA.CFG has lost its CRLF line endings; the engine "
                 "cannot parse it that way (see the comment at its top)")

    print(f"{len(plan)} files staged, {total:,d} bytes copied. Now: make iso")


if __name__ == "__main__":
    main()
