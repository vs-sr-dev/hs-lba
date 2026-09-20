#!/usr/bin/env python3
"""
count_blits.py — how many AffGraph calls does a real AffGrille full redraw make?

That count times the measured per-blit cost (33.1 us on HyperScan under MAME,
milestone m4) is the full-redraw budget, which is the number the whole port
hinges on. Replicates the engine's own data path exactly:

  LBA_GRI entry -> per-column RLE (DecompColonne) -> 64x64x25 cube of
  (block, brick) byte pairs; a cell draws when block != 0 and the block's
  brick entry is non-zero (GRILLE.C AffBrickBlock), and when Map2Screen puts
  it inside the -24..640 / -38..480 window (GRILLE_A.C).
"""

import struct
import sys

SIZE_CUBE_X = 64
SIZE_CUBE_Y = 25
SIZE_CUBE_Z = 64
HEADER_BLOCK = 3


def hqr_entry(data, index):
    n = struct.unpack_from("<I", data, 0)[0] // 4
    if index >= n:
        return None
    off = struct.unpack_from("<I", data, index * 4)[0]
    size, csize, method = struct.unpack_from("<IIH", data, off)
    payload = data[off + 10: off + 10 + csize]
    if method == 0:
        return bytes(payload[:size])
    if method != 1:
        return None
    out = bytearray()
    i = 0
    count = size
    while count > 0:
        flags = payload[i]; i += 1
        for _ in range(8):
            if flags & 1:
                out.append(payload[i]); i += 1; count -= 1
                if count == 0:
                    return bytes(out)
            else:
                tok = payload[i] | (payload[i + 1] << 8); i += 2
                ln = (tok & 0x0F) + 2
                back = len(out) - (tok >> 4) - 1
                count -= ln
                for k in range(ln):
                    out.append(out[back + k])
                if count <= 0:
                    return bytes(out)
            flags >>= 1
    return bytes(out)


def decomp_colonne(src, pos):
    """GRILLE_A.C DecompColonne: yields SIZE_CUBE_Y u16 cells."""
    out = []
    nb = src[pos]; pos += 1
    while nb > 0:
        al = src[pos]; pos += 1
        cl = (al & 0x3F) + 1
        if (al & 0xC0) == 0:
            out.extend([0] * cl)
        elif al & 0x40:
            for _ in range(cl):
                out.append(src[pos] | (src[pos + 1] << 8)); pos += 2
        else:
            v = src[pos] | (src[pos + 1] << 8); pos += 2
            out.extend([v] * cl)
        nb -= 1
    out.extend([0] * (SIZE_CUBE_Y - len(out)))
    return out[:SIZE_CUBE_Y]


def build_cube(gri):
    cube = [[[0] * SIZE_CUBE_Y for _ in range(SIZE_CUBE_X)]
            for _ in range(SIZE_CUBE_Z)]
    for z in range(SIZE_CUBE_Z):
        for x in range(SIZE_CUBE_X):
            off = struct.unpack_from("<H", gri, (x + z * SIZE_CUBE_Z) * 2)[0]
            cube[z][x] = decomp_colonne(gri, off)
    return cube


def block_draws(bll):
    """For each block, the set of brick slots that actually draw."""
    nblocks = struct.unpack_from("<I", bll, 0)[0] // 4
    draws = []
    for b in range(nblocks):
        off = struct.unpack_from("<I", bll, b * 4)[0]
        base = off + HEADER_BLOCK
        slots = set()
        for brick in range(64):
            p = base + brick * 4 + 2
            if p + 2 > len(bll):
                break
            if struct.unpack_from("<H", bll, p)[0]:
                slots.add(brick)
        draws.append(slots)
    return draws


def visible(xm, ym, zm):
    xs = (xm - zm) * 24 + 288
    ys = (xm + zm) * 12 - ym * 15 + 215
    return -24 <= xs < 640 and -38 <= ys < 480


def main():
    scene = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    gri = hqr_entry(open("data/LBA_GRI.HQR", "rb").read(), scene)
    bll = hqr_entry(open("data/LBA_BLL.HQR", "rb").read(), scene)
    if not gri or not bll:
        print("scene not found")
        return 1

    cube = build_cube(gri)
    draws = block_draws(bll)

    cells = []          # (x, y, z) that would issue a blit if on screen
    for z in range(SIZE_CUBE_Z):
        for x in range(SIZE_CUBE_X):
            col = cube[z][x]
            for y in range(SIZE_CUBE_Y):
                v = col[y]
                block, brick = v & 0xFF, (v >> 8) & 0xFF
                if block and (block - 1) < len(draws) and brick in draws[block - 1]:
                    cells.append((x, y, z))

    print(f"scene {scene}: {len(cells)} drawable cells in the cube "
          f"({len(draws)} blocks)")

    # Sweep the camera over the grid; count what lands on screen.
    counts = []
    for sz in range(0, SIZE_CUBE_Z, 2):
        for sx in range(0, SIZE_CUBE_X, 2):
            n = 0
            for (x, y, z) in cells:
                if visible(x - sx, y, z - sz):
                    n += 1
            counts.append(n)

    counts.sort()
    mean = sum(counts) / len(counts)
    p90 = counts[int(len(counts) * 0.9)]
    print(f"blits per full redraw:  mean {mean:.0f}   p90 {p90}   max {counts[-1]}")
    print()
    print(f"{'blits':>8}{'ms @33.1us':>13}{'fps':>8}")
    for n in (int(mean), p90, counts[-1]):
        ms = n * 33.1 / 1000.0
        print(f"{n:>8}{ms:>13.1f}{1000.0 / ms if ms else 0:>8.1f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
