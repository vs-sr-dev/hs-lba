# Little Big Adventure — Mattel HyperScan

A port of LBA1 (from the GPL Adeline sources, *lba1-classic-community*) to
the **Mattel HyperScan**, 2006: a Sunplus SPG290 SoC with a S+core 7 CPU at
108 MHz, 16 MB of RAM, a bare CD mechanism driven through a servo DSP, an
RGB565 TV encoder, a DAC FIFO for sound — and, as its only writable
storage, **96 bytes** on an RFID trading card.

It is the console that Mattel sold for one Christmas and then dropped, the
one whose games were famous for their loading times. It runs LBA at its
native **640×480**, from the CD data, with the movies, the sound effects, the
spoken dialogue, and a save system on the card.

→ **[DEVLOG.md](DEVLOG.md)** — the things that were not obvious: what the
SDK never turns on, what the emulator gets wrong, and why the renderer was
calling libc to move two bytes.
→ **[docs/RFID-SAVES.md](docs/RFID-SAVES.md)** — how a game is saved on a
console whose only writable storage is 96 bytes on a trading card: the tag,
the pin, the protocol, the two slots, and what the player has to do.
→ **[mame/README.md](mame/README.md)** — stock MAME cannot run this; the
patches, the fork, the build, and what a prebuilt binary owes the GPL.

## Status

Everything below was measured under MAME; there is no physical HyperScan in
this project. MAME's S+core core is about 6× pessimistic on the CPU (six
cycles per instruction, no cache model) and optimistic on the disc (seeks
are free), so real hardware should be faster in the frame and slower in the
loads — the DEVLOG says where each number came from.

| Milestone | What it proves | State |
|---|---|---|
| **m1 — toolchain, first pixel** | the official Sunplus GCC 4.2.1 builds valid code and a framebuffer comes up: the sixteen documented "score-elf bugs" are absent from its output (`tests/torture/`) | ✅ |
| **m2–m3 — the disc** | servo DSP driver, a flat archive with an index sector, the first HQR loaded and checksummed against the host | ✅ |
| **m5 — input** | the SDK's I²C driver deadlocks on its first byte (two bugs); a replacement reads 10 buttons and the stick with 0 timeouts in 16,660 transfers | ✅ |
| **m6 — interrupts** | the SDK never enables them. `EXCPVEC` and `PSR.IE` set by hand, a 50 Hz timer that is exactly 50 Hz (125 kHz / 2500), vblank at 29.97 Hz | ✅ |
| **m7 — stdio** | the engine's `FILES.C` compiles untouched: newlib's bottom end sits on the disc archive. 11 files, 8.7 MB read through `fread`, 11/11 checksums match | ✅ |
| **m8 — the engine links** | 59 engine files under GCC, 71 platform symbols, a printf ring buffer in RAM because the UART goes nowhere. First boot: a `LBA.CFG` with LF line endings sent the engine into `exit(1)` | ✅ |
| **m9 — the menu** | `#pragma pack` does not exist on this target; `Log` byte-identical to the host after fixing eleven on-disk structs. Palette fades emulated as a DAC | ✅ |
| **m10 — in game** | pad polled from the vblank ISR like the DOS keyboard IRQ. The ISR stub was not saving the hardware loop counter: a `memcpy` that ran 2³² times | ✅ |
| **m11 — movies** | Watcom packed at `-zp2`, not to the byte: the FLA header has a pad the file contains. All 23 movies play | ✅ |
| **m12 — sound** | DAC FIFO driven the way the PPCSDK's `MP3Drv.c` drives it, DOS `WAVE.C` mixer semantics, mixing inside the interrupt. The CD ring was one sector deep | ✅ |
| **m13 — prefetch** | the servo runs ahead into a 512-sector ring, parked by its own interrupt. Intro movie 32 s → 20 s, stalls 2,900 → 0 | ✅ |
| **m14 — voices** | zero new lines in the audio path; 12 VOX banks on the disc; 11,111 Hz, LZSS, `'C'` of the VOC magic overwritten by a flag | ✅ |
| **m17–m18 — the pools and the leaks** | `Malloc(-1)` is a memory *query*; half a megabyte and 860 ms to load a 5 KB sample; three "separate" bugs that were eight file handles | ✅ |
| **m19 — saving** | 96 bytes on a Topaz RFID tag, bit-banged on one GPIO pin, calibrated against the 50 Hz tick. Autosave in RAM, manual save to the card, the engine's own load list unmodified | ✅ |
| **m20 — controls** | the DOS keyboard laid over the pad; behaviour switching on the shoulders through a mailbox, because `SetComportement()` reloads a body and cannot run in an interrupt | ✅ |
| **m21 — a frame rate, at last** | a sampling profiler that charges `memcpy` to its caller through `r3`; `RW()` was `memcpy(&v, p, 2)`. **38.7 → 49.1 fps**, +27 % in one file | ✅ |
| **m22 — geometry, not fill** | 18.8 ms a frame + 14.5 ms an object: screen area predicts nothing. 640×480 output costs 6.7 % in game; fades cut, resolution follows the source; boot to first scene **395 s → 128 s** | ✅ |
| music | LBA1's CD audio tracks; no path for them yet | ⏳ |
| LOD and pre-sort culling | the two levers left on the 53 % of the frame that is per-vertex work | ⏳ |
| save polish | custom on-screen strings for the card flow; watch `save_flag_exceptions` over a full playthrough | ⏳ |
| mastering | an ISO9660 wrapper so a real console's BIOS finds `HYPER.EXE`; nothing here has ever run on hardware | ⏳ |

**In one number: 96.** That is how many bytes the console can write — the
user area of the Topaz tag inside a HyperScan trading card — and a minimal
LBA1 save is 468. The three arrays that are almost all of the state
become bitmaps — `ListFlagGame` 32 bytes, `TabHoloPos` 19, the inventory 4 —
and the flags a life script sets to a value other than 0 or 1 go in a
seven-slot exception list. An eighth **refuses the save** rather than
truncating it. Nobody has surveyed which of the 255 flags are counters; the
port counts them while you play.

**In one line of disassembly.** Twinsen's cell, one actor, 38.7 fps with 0 %
headroom, and `memcpy` at 19 % of the profile:

```
RW:  push r3 / addi r0,-16 / jl memcpy / lhu r4,[r0,12] / extsh / br r3
```

`RW()` reads a WORD from model data and was written `memcpy(&v, p, 2)`, the
textbook unaligned-read idiom. score-elf-gcc at `-Os` does not fold it: a
stack frame and two nested calls into libc, per field, per vertex, per
polygon, per actor. Assembling the two bytes by hand is byte-identical
output and keeps the property the original was written for. 49.1 fps.

## Bring your own assets

There is **no game data in this repository**. You need your own copy of
*Little Big Adventure* — the GOG release is what this is developed against —
and the staging script copies what the port needs into `data/`:

```sh
python tools/stage_data.py "path/to/Little Big Adventure"   # the GOG install root
```

It takes the **CD** data from `Common/` (the 640×480 archives, the FLA
movies, the `EN_*.VOX` voice banks) and `TEXT.HQR` from `CommonClassic/`,
where GOG keeps it on its own. Do not point it at `Speedrun/Windows/`: that
is the DOS floppy set the DS port uses, with the same filenames. One
language of voices ships — twelve archives are 32 MB, five languages would
be 160.

`data/LBA.CFG` is the one file in `data/` that *is* in the repository. It is
a config file written for this console, not an asset, and its line endings
are CRLF on purpose — the reason is at the top of it.

Two more things are yours to supply, for the same reason: the **toolchain**
(Sunplus' GCC 4.2.1 — [toolchain/README.md](toolchain/README.md) has the
extraction recipe) and the **console BIOS** for MAME.

## Build

The Sunplus toolchain in `toolchain/gnu/`, MSYS2's `make` (the 3.79 that
ships with the toolchain does not do order-only prerequisites), Python 3.

```powershell
$env:PATH = "C:\msys64\usr\bin;" + $env:PATH
make            # build/HYPER.EXE, plus the symbol tables the harnesses read
make iso        # build/hslba.iso — the data archive, in load order
```

Run it from PowerShell with MSYS on `PATH`: the `HYPER.EXE` rule ends in
`ls -l`, and without it `make` dies *after* objcopy has written the image
but before `build/syms.lua` is regenerated — the binary looks fine and
every harness reads stale addresses.

One-subsystem test programs replace the entry point:

```sh
make MAIN=src/m19_card.c      # the RFID driver alone
```

Build knobs, all `-D` through `EXTRA_CFLAGS`:

| | |
|---|---|
| `HS_VIDEO_QVGA` | 320×240 output instead of 640×480, for when a real TV says the interlace flickers |
| `HS_VIDEO_FIXED_RES` | never switch the TV encoder's mode: MAME cannot show what an NTSC set does on a 480i↔240p change |
| `HS_KEEP_FADES`, `HS_KEEP_EA_LOGO`, `HS_KEEP_INTRO_SLIDES` | put back the three things the boot sequence dropped to go from 395 s to 128 s |
| `HS_CD_NO_PREFETCH` | park the servo after every read — the A/B control for m13 |
| `HS_I2C_NO_INT_EN` | read the pad on the ACK bit alone, proving the driver does not depend on MAME's interrupt model |
| `HS_SHOULDER_DIRECT` | one behaviour per shoulder button instead of a ring |

## Run

MAME with the `hyprscan` driver — **patched**: stock MAME aborts on the first
rotate instruction and has no sound device. The patches, the build command
and the licensing of a prebuilt binary are in [mame/README.md](mame/README.md).

```
hyprscan hyprscan -rompath "<dir with hyprscan.zip>;build" \
    -quickload build/HYPER.EXE -cdrom build/hslba.iso \
    -memc build/card_blank.bin -ctrlrpath tools -ctrlr lba-keys
```

`python tools/mkcard.py build/card_blank.bin` makes the card. MAME does not
write the `.bin` back: the card lives in `nvram/hyprscan/`, and starting
over means deleting that too. **To save the game:** put the card on the
reader (mount it, under MAME) *before* choosing Save Game, hold it there
until "Game saved", and know that everything since your last save lives in
RAM only — the `*` next to Save Game says so. The whole mechanism is in
[docs/RFID-SAVES.md](docs/RFID-SAVES.md).

## Controls

The pad has no d-pad: four coloured buttons, Start, Select, two shoulders,
two triggers and one stick. The mapping is the DOS keyboard, one key per
button; `tools/lba-keys.cfg` is the inverse, so under MAME you play with the
keys the manual names.

| pad | DOS key | in game |
|---|---|---|
| stick | arrows | move |
| green | Space | action — talk, take, jump, hit, hide, by behaviour |
| yellow | Enter | recentre the camera; confirm in menus |
| blue | Alt | use the weapon / throw the magic ball |
| red | Ctrl (held) | the status screen: life, magic, coins, keys |
| Select | Shift | inventory |
| Start | Esc | pause / save menu |
| L1 / R1 | — | behaviour, one step left / right |
| L2 / R2 | 1 / 2 | magic ball / sword |

The holomap has no button of its own: six keys are worth a button and six
buttons are not shoulders. It is the first item of the inventory, one press
further than the keyboard. Under MAME, Esc is remapped to Backspace or the
emulator quits when the pause menu opens.

## Tools

Everything in `tools/` answers a question the machine will not answer any
other way. The `m*.lua` files are MAME autoboot scripts: they read engine
globals by name through `build/syms.lua`, inject input, and assert on RAM
rather than on screenshots.

| | |
|---|---|
| `stage_data.py` | copies the game files out of your install |
| `mkcd.py` | the data image: a flat archive with an index sector, files contiguous in load order, FNV-1a checksums the target verifies without a host |
| `mkcard.py`, `cardinfo.py` | a blank Topaz card image for MAME, and a reader for the 96-byte save it ends up holding |
| `m5_input_test.lua`, `m6_timer_test.lua`, `m7_fs_test.lua` | the platform layer, one subsystem each: pad bytes, tick rate, stdio checksums |
| `m12_audio.lua`, `m13_cd.lua`, `m14_voice.lua` | fills per second, sectors per seek, stalls per movie; the four places a voice can silently fail |
| `m18_fetch.lua` | per-file read accounting — which archive cost that hitch — and the names of the eight handles when they run out |
| `m19_card.lua`, `m19_save.lua` | the card driver round-trip, and the engine's own autosave succeeding |
| `m20_behaviour.lua` | the shoulders switch behaviour without the menu ever appearing |
| `m21_profile.lua` | a sampling profiler: PC per video frame, charged to the enclosing function, `memcpy` charged to its caller through `r3` |
| `m22_scenes.lua` | writes `NewCube` to visit any scene on the disc and measure it; frame cost against object count |
| `lba-keys.cfg` | the keyboard profile for MAME |
| `count_blits.py`, `host3d.c` | the host-side halves of two investigations |

## Repository map

```
src/          the platform: boot, interrupts, timer, video, input, CD, fs,
              audio, card, save — and one m*.c per single-subsystem test
translate/    Adeline's x86 assembly, already in C (from the DS port)
compat/       what a 1994 Watcom source needs to compile under GCC
engine/       the LBA1 engine, with the port's edits behind PORT_HS
sdk/          the 28 files of the HyperScan SDK the build uses, two of them fixed
mame/         patches against MAME 0.287, and what a prebuilt binary owes the GPL
tests/        the toolchain torture test; the save-format round-trip (host)
tools/        harnesses and mastering scripts
docs/         RFID-SAVES.md, the card emulation write-up; Sunplus' manuals go here, gitignored
data/         your game files (gitignored), plus LBA.CFG
toolchain/    Sunplus' GCC goes in gnu/ (gitignored)
```

## Where the engine comes from

`engine/` is the *lba1-classic-community* source as released by Adeline
Software International under the GPL, carried over from the
[DS port](https://github.com/vs-sr-dev/ds-lba) together with `translate/`,
which is that port's translation of the x86 assembly into C. This port's
edits are guarded with `PORT_HS` where the earlier ones used `PORT_NDS` and
`PORT_SDL`, plus the on-disk struct fixes (`PORT_PACKED`), the disabled
`CloseFdNar` re-enabled, and the small-copy rewrites in `translate/`. The
DEVLOG records each with its reason.

## Credits and licences

The LBA1 engine is © Adeline Software International, released under the
GPL as *lba1-classic-community*; this repository is under the same licence
([LICENSE](LICENSE)). The game data is © Adeline / [2.21] and is not
distributed here. The SDK slice in `sdk/` comes from ppcasm's
[HyperScan-SPG29x-SDK](https://github.com/ppcasm/HyperScan-SPG29x-SDK), and
the toolchain and manuals are Sunplus'. The hardware facts were checked
against MAME's `spg29x` driver and, where MAME was wrong, against the
PPCSDK's own driver sources; the earlier findings on the platform are in
[hyperscan-homebrew](https://github.com/vs-sr-dev/hyperscan-homebrew).
