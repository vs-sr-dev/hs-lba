# mame/ — the emulator this port is developed on

There is no physical HyperScan in this project. Every number in the DEVLOG
was measured under MAME's `hyprscan` driver — and **stock MAME cannot run
the port**: it aborts on the first rotate instruction the compiler emits,
and it has no sound device for this chip. Three changes fix that. They are
small, they are here as patches against a named release, and they are also
applied as commits on a public branch, so that a prebuilt binary always has
its complete source next to it.

**The short version:**

| you want | do this |
|---|---|
| just run the game | download `hyprscan.exe` from the [releases](https://github.com/vs-sr-dev/hs-lba/releases) of this repository, put Mattel's `hyprscan.zip` BIOS somewhere on `-rompath`, follow *Running* below |
| build the emulator yourself | clone the branch [`hs-lba-0287`](https://github.com/vs-sr-dev/mame/tree/hs-lba-0287) of the fork and build the `hyprscan` subtarget — *Building*, below |
| apply the changes to your own MAME tree | `patch -p1` the three files in this directory onto MAME **0.287** — *The patches*, below |

## The patches

Against MAME 0.287 (`mame0287` tag). They apply in order with `patch -p1`
from the MAME root, and were verified with `patch --dry-run` against the
pristine files at the time of writing.

| patch | what | why the port needs it |
|---|---|---|
| `0001-score-rotates.patch` | implements `ror`/`rol`/`rori`/`roli` and the four carry variants in the S+core CPU core (`src/devices/cpu/score/score.cpp`) | upstream calls `unemulated_op()` for all eight — a **fatal error**, not a warning. GCC 4.2.1 emits `rori` for any ordinary C rotate idiom, and LBA's LZSS decoder, hashes and PRNGs all rotate. The register-count forms are verified against a bit-at-a-time reference by `src/rotate_test.c`; the carry forms are implemented but no compiler emits them |
| `0002-spg290-spu.patch` | **adds** a `spg290_spu` device (`src/devices/machine/spg290_spu.cpp/.h`, BSD-3-Clause), registers it in `scripts/src/machine.lua`, maps it at `0x08050000` in `src/mame/tvgames/spg29x.cpp` and routes it to two speakers | upstream has the SPU commented out: `//map(0x08050000, 0x0805ffff); // SPU`. The device models the **DAC FIFO** the way the only shipped first-party code drives it (`MP3Drv.c` in the PPCSDK): interleaved stereo frames, sample clock = 27 MHz / (N+1), half-buffer ping-pong with an interrupt on vector 63 — plus the 24 IMA-ADPCM hardware channels, which this port does not use. The same patch maps a card-detect word at `0x08200070` that the port **deliberately ignores**: on real silicon that address is `P_IOB_GPIO_INPUT` (see [docs/RFID-SAVES.md](../docs/RFID-SAVES.md) §2) |
| `0003-hyperscan-card-resp-idx.patch` | `m_resp_idx` in `src/mame/tvgames/hyperscan_card.h` from `uint8_t` to `uint16_t` | a `RALL` answer from the RFID tag is 1,114 bits and the half-bit counter wraps at 256, so the emulated card repeats its first ~12 bytes forever. **Optional**: `src/card.c` detects the truncation and falls back to 96 single `READ`s, so the game is correct without it — a load just costs 1.26 s instead of 0.38. [docs/mame-card-resp-idx.patch](../docs/mame-card-resp-idx.patch) has the full write-up |

Nothing else in MAME is touched. In particular the CPU timing (`m_icount -= 6`
per instruction, no cache model) and the CD servo (seeks take zero time) are
left as upstream has them: both are wrong in ways the DEVLOG accounts for,
and "fixing" them in the emulator would only hide the cost from the code
that has to pay it on the console.

## Building

The fork branch is the three patches applied as three commits on top of
`mame0287`, nothing more:

```
https://github.com/vs-sr-dev/mame/tree/hs-lba-0287
```

On Windows, MAME builds under **MSYS2** with the MinGW-w64 toolchain. Install
the packages MAME's own guide lists
([docs.mamedev.org → Compiling MAME](https://docs.mamedev.org/initialsetup/compilingmame.html));
for 0.287 that is, from the *MSYS2 MinGW 64-bit* shell:

```sh
pacman -S git make mingw-w64-x86_64-gcc mingw-w64-x86_64-python \
          mingw-w64-x86_64-SDL2 mingw-w64-x86_64-libslirp
```

Then:

```sh
git clone --depth 1 --branch hs-lba-0287 https://github.com/vs-sr-dev/mame mame-hslba
cd mame-hslba
make SUBTARGET=hyprscan SOURCES=src/mame/tvgames/spg29x.cpp -j8 NOWERROR=1
```

`SUBTARGET=hyprscan SOURCES=…` builds an emulator containing **only** the
HyperScan driver and the devices it pulls in. It is a fraction of a full MAME
build — still expect a good while and a few gigabytes of object files on a
first run — and produces `hyprscan.exe` (about 110 MB) in the tree root.
`NOWERROR=1` stops a compiler warning from failing the build, which on a
compiler version upstream does not test against it otherwise can.

Or, from a pristine upstream tree instead of the fork:

```sh
git clone --depth 1 --branch mame0287 https://github.com/mamedev/mame
cd mame
for p in /path/to/hs-lba/mame/*.patch; do patch -p1 < "$p"; done
make SUBTARGET=hyprscan SOURCES=src/mame/tvgames/spg29x.cpp -j8 NOWERROR=1
```

Linux and macOS build the same way with their native toolchains; nothing in
the patches is Windows-specific.

## Running

The console BIOS — `hyprscan.zip`, containing `hyperscan.bin` — is Mattel's
and is not distributed here. MAME wants it in a directory on `-rompath`.

```
hyprscan hyprscan -rompath "<dir with hyprscan.zip>;build" \
    -quickload build/HYPER.EXE -cdrom build/hslba.iso \
    -memc build/card_blank.bin \
    -ctrlrpath tools -ctrlr lba-keys
```

| option | what it does |
|---|---|
| `-quickload build/HYPER.EXE` | loads the image at `0xA00901FC` and enters at `0xA0091000`, which is what the BIOS would do from a disc; `build` is on `-rompath` so MAME finds it |
| `-cdrom build/hslba.iso` | the data image from `make iso` — a flat archive, not ISO9660 (see `tools/mkcd.py`) |
| `-memc build/card_blank.bin` | the RFID card, from `tools/mkcard.py`. MAME persists it in `nvram/hyprscan/card_blank.nv`, **not** in the `.bin`; delete the `.nv` to start from a blank card |
| `-ctrlr lba-keys` | lays the DOS keyboard over the pad (README, *Controls*); also moves MAME's quit key off Esc, which the game uses |

For the harnesses in `tools/`, add
`-autoboot_script tools/<name>.lua -autoboot_delay 0`; they read engine
globals by name through `build/syms.lua`, so `make` first. For an unattended
measurement run, `-nothrottle -video none -seconds_to_run N` is the usual
trio.

## The GPL question

MAME is GPL-2.0-or-later, with BSD-3-Clause parts. Distributing a **modified
binary** obliges the distributor to make the *corresponding source*
available — the whole source of the thing that was built, not just the
delta. This repository does it in the way that is beyond argument under
either licence version:

1. **The complete source is public, as built.** The `hs-lba-0287` branch of
   [vs-sr-dev/mame](https://github.com/vs-sr-dev/mame) is exactly the tree the
   binary comes from: upstream `mame0287` plus three commits. That is the
   "corresponding source" of GPLv2 §3 and GPLv3 §6, available from a server
   clearly designated by the release notes.
2. **The delta is here too**, as patches, for anyone who wants to read the
   change rather than diff two trees.
3. **The binary is a release asset, not repository content.** It is 110 MB,
   which is over GitHub's 100 MB per-file limit for a repository anyway, and
   every release that carries it names the branch and the commit it was
   built from.

The new file in patch 0002 (`spg290_spu.cpp/.h`) is BSD-3-Clause like the
`spg290_*` devices beside it; the modifications to `score.cpp`,
`spg29x.cpp` and `hyperscan_card.h` keep those files' own licence headers.
