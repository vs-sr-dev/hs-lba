# Saving on a console with no storage: the RFID card

The HyperScan has no memory card slot, no EEPROM for games, no writable
disc. What it has is the thing it was sold on: a 13.56 MHz RFID reader in
the lid, and trading cards with a tag inside. Mattel's games read those cards
to unlock characters and wrote a few bytes back to level them up. That is the
only writable storage on the machine, and this port saves the game on it.

This document is the whole mechanism, from the pin to the pause menu. If you
only want to play: **put a card on the reader before you choose Save Game,
hold it still for up to two seconds, and take it off when the game says
"Game saved".** Everything else here is why.

---

## 1. The tag

The card holds an **NXP/Innovision Type 1 tag — a Topaz/Jewel** (ISO 14443-3
Type A framing, Type 1 command set). It has 120 bytes of EEPROM laid out
like this:

| offset | bytes | what | writable |
|---|---|---|---|
| 0x00–0x06 | 7 | UID | no |
| 0x07 | 1 | reserved | no |
| **0x08–0x67** | **96** | **user data** | **yes** |
| 0x68–0x6F | 8 | reserved | no |
| 0x70–0x71 | 2 | lock bytes | one-way: setting a bit locks a block forever |
| 0x72–0x77 | 6 | OTP | one-way |

**A save has 96 bytes.** Not 120: the port never addresses anything outside
0x08–0x67. MAME's model of the card lets a program write all 120, including
the UID and the lock bytes; real silicon does not, and writing a lock bit on
a real card is permanent. This is the first of several places where the
emulator is more permissive than the hardware and the code has to be
stricter than the emulator requires.

The commands the port uses, out of the Type 1 set:

| command | does | cost (measured in the port's own timing) |
|---|---|---|
| `REQA` | "is there a card?" — 7 bits out, a 2-byte answer | ~1 ms |
| `RID` | read the UID | small |
| `RALL` | **read all 120 bytes in one transaction** | ~370 ms |
| `READ` | read one byte | ~13 ms each; 96 of them ≈ 1.26 s |
| `WRITE-E` | erase-and-write **one byte** | 71 bits out, plus the tag's own EEPROM programming time, ~8 ms |

Two asymmetries in that table decide the whole design: **reading everything
is one transaction, writing anything is one transaction per byte.** A full
save is up to ~96 `WRITE-E`s and about a second of unbroken contact. A full
load is one `RALL`.

## 2. The wire

There is no RFID controller. The SPG290 talks to the tag by **bit-banging
one GPIO pin**: bit 1 of `P_CSI_GPIO_SETUP` (0x88200024) drives the antenna,
and the tag's answer comes back in `P_CSI_GPIO_INPUT` (0x88200068). The same
setup register carries the eight front-panel LEDs on bits 5–12, which is how
the output-enable mask was found — it is exactly what the SDK's `HS_LEDS()`
macro writes, the one pin configuration the board is known to accept.

Only the **high time** of a pulse carries meaning, measured in cycles of the
13.56 MHz carrier:

| high time | meaning |
|---|---|
| < 500 cycles (< 36.9 µs) | a 0 bit |
| 500–999 (36.9–73.7 µs) | a 1 bit |
| 1000–4999 (73.7–368.7 µs) | a *read strobe*: advance the tag's answer by one half-bit |
| ≥ 5000 | ignored |

The low time between pulses is unconstrained, and that is the one piece of
luck in this. Slow code between bits is free; only the pulse itself has to be
timed. So `src/card.c` disables interrupts **around each pulse**, not around
the transaction: a transaction-wide mask would mean a second and a half with
the 50 Hz tick and the CD servo dead, whereas per pulse costs at most 165 µs
of interrupt latency and leaves a gap after every bit.

Answers are Manchester coded — each bit presented twice, the second inverted,
so one logical bit is two strobes. Every answer opens with fifteen 0 bits and
a 1, which is what the receiver hunts for; each byte after that is eight bits
LSB-first plus odd parity. A `RALL` answer is 2 preamble bytes + (HR0, HR1,
120 data bytes, 2 CRC) × 9 bits = **1,114 bits, 2,228 strobes**. The strobe
is the expensive primitive, and reading is what strobes are for — which is
why reading costs far more than writing per byte, and why the save reads
the whole card twice rather than reading back each byte's echo (96 echoes:
1.6 s, proving only that the tag repeated what it was told; one `RALL`:
370 ms, proving what the tag now holds).

**The pulse widths are calibrated at runtime.** There is no cycle counter to
trust on both the emulator and the console, so `card_init()` times a delay
loop against the 50 Hz timer interrupt — the one clock that is correct in
both places — and every width is derived from that. Each width sits near the
geometric centre of its window rather than the arithmetic one, because the
error being guarded against is a *ratio*: a miscalibrated loop is
proportionally fast or slow, not off by so many microseconds. Bit 0 at
18 µs, bit 1 at 52 µs, strobe at 165 µs.

**The register MAME offers and the port refuses.** MAME's `hyprscan` driver
maps a one-word "card present" register at `0x08200070` that returns 1 when
a card image is mounted. That address is `P_IOB_GPIO_INPUT` on real silicon,
and the MAME source itself labels the mapping `homebrew extension`. Reading
it would make card detection work in the emulator and nowhere else, so
`card_present()` sends a real `REQA` instead. Same reasoning for programming
time: MAME's `WRITE-E` completes instantly, a real tag wants ~5 ms of EEPROM
programming per byte, and `T_PROGRAM_US` in `card.c` spends that time
whether the emulator needs it or not. Leaving it out would make the emulated
save four times faster than the real one and hide the fact that the player
has to hold the card still for over a second.

## 3. The API is shaped by the cost

```c
int card_present(void);                       /* one REQA, ~1 ms          */
int card_read_user(unsigned char buf[96]);    /* RALL, or 96 READs        */
int card_write_begin(const unsigned char *want);   /* -> steps to come     */
int card_write_step(void);                    /* programs ONE byte         */
int card_write_finish(void);                  /* re-read, verify, magic    */
```

There is deliberately **no `card_write(buf)` that returns success**. Such a
function can only be written in an emulator: it would compile, pass every
test, and then fail on a console the first time somebody lifted the card
early. The three-phase shape forces the caller to know that a write is a
process that takes a second and can be interrupted.

What the three phases do:

1. **`begin`** reads the card, diffs it against the 96 bytes wanted, and
   **clears the magic byte first**. From this moment until `finish`, the card
   reads as *empty*, never as a save that is half old and half new.
2. **`step`** programs one differing byte per call. A second save of the same
   game is a handful of bytes, not 96: the harness measured 6 bytes instead
   of 95 on a re-save.
3. **`finish`** reads the whole card again (`RALL`), compares it with what was
   wanted, and **only then writes the magic back**. A save that is on the
   card is a save that was verified there.

`RALL` under MAME — see §7 — may come back truncated. `card_read_user()`
detects it and falls back to 96 single `READ`s (1.26 s instead of 0.38);
`card_rall_fallbacks` counts how often.

## 4. Two slots, one format

The engine has exactly one serialiser — `SaveGameWithName()` in `GAMEMENU.C`
— and in its *autosave* form it emits a **468-byte** stream. The port keeps
that stream in two places and lets the engine reach both the way it reaches
everything else, through `fopen`:

| virtual file | where | written | survives power-off |
|---|---|---|---|
| `AUTOSAVE.LBA` | a buffer in RAM | on every scene change and every death, exactly as the DOS game wrote its file | **no** |
| `CARD.LBA` | 96 packed bytes on the card | only when the player chooses Save Game with a card on the reader | yes |

`_open_r` in `src/syscalls.c` serves these two names before it looks at the
disc archive; `SYS_FindFirst` in `src/platform.c` enumerates exactly them,
so the engine's own Load Game list works unmodified and shows both.

Why not write the card on every scene change? Because of the second column
of the table in §1: a save is ~92 transactions and a second of contact, and
LBA1's scene changes are not chapter boundaries — the island changes thicken
towards the end of the game. Writing on them would bring back exactly the
stall the design exists to avoid, and would require the card to be on the
reader at moments the player did not choose. So:

- **Every scene change → RAM only.** Free, cannot fail, needs no card. It is
  what makes the DOS game's "Error Writing Saved Game" disappear, and it
  covers what autosave is for in LBA — dying and continuing.
- **Save Game → the card.** Synchronous, with the card *already* resting on
  the reader. The player places the card, then chooses the menu entry; there
  is no modal "waiting for card" loop.
- **Load → `RALL`.** One transaction, instant by construction.

The contract that follows, and that the menu makes visible: **progress is
valid for the session until the player saves.** The RAM slot carries a
generation counter compared against the last successful card write, and
`save_unsaved_progress()` is true whenever RAM is newer than the card.

## 5. Ninety-six bytes

468 do not fit in 96. Almost all of the 468 is three arrays that are morally
bitmaps and physically byte arrays:

| engine array | bytes | packed |
|---|---|---|
| `ListFlagGame[255]` | 255 | 32 (one bit: non-zero) |
| `TabHoloPos[150]` | 150 | 19 (one bit) |
| `ListFlagInventory[28]` | 28 | 4 (one bit) |

`TabHoloPos` packs losslessly: `HOLOMAP.C` only ever stores 0, 129 or 64, and
the sole read of bit 64 is commented out. `ListFlagInventory` is only ever
set to TRUE.

**`ListFlagGame` does not pack losslessly, and this is the part worth being
honest about.** `GERELIFE.C`'s `LM_SET_FLAG_GAME` copies a byte straight out
of a life script, so a flag may legitimately hold any value; which of the 255
are counters rather than booleans is a property of the game's scripts, and
nobody here has surveyed them. Guessing "all boolean" would corrupt a save
quietly, months from now, in whichever scene first uses a counter. So the
bitmap records "non-zero", and any flag holding something other than 0 or 1
is *also* written in full to an **exception list of seven (index, value)
pairs**. If a save ever needs an eighth, **the save is refused** — "Save
failed" on screen — rather than silently truncated, and `save_flag_exceptions`
keeps the high-water mark. Playing the game is what tells us how many are
really needed; if that number approaches seven, the format needs more room,
not a shrug.

The layout, at card offset 0x08 (`src/save.c`, mirrored by
`tools/cardinfo.py`):

```
 0  magic      'L' when valid, 0 while a write is in progress — written LAST
 1  version    1
 2  checksum
 3  ListFlagGame bitmap        32 bytes
35  TabHoloPos bitmap          19
54  ListFlagInventory bitmap    4
58  NumCube, Chapitre, Comportement, Life
62  Gold (2)
64  MagicLevel, MagicPoint, CloverBox, Clover, Fuel
69  Weapon (2), StartX/Y/Z (2 each), Beta (2)
79  GenBody
80  exception count
81  exceptions: 7 × (flag index, value)
95  reserved
```

**What a card save does not carry.** The DOS *manual* save also wrote
`ListObjet`, `ListExtra`, the zone list and the per-cube flags — several
kilobytes that no packing brings near 96. A card save therefore restores what
an autosave does: the scene and the player's state, not the exact arrangement
of the objects in the room being left. That is a consequence of 96 bytes,
not a shortcut, and it is why Save Game writes the *autosave* form on purpose
rather than a bigger one it would then mostly drop.

## 6. What the player sees

The pause menu (`GAMEMENU.C`, behind `PORT_HS`):

- **Save Game** carries `Card OK` or `No card`, right-aligned inside the same
  button, sampled with a `REQA` every half second — attached to the option
  rather than parked in a corner, because what it really says is "this will
  work if you do it now", and a status line elsewhere reads as a fault
  report. A trailing `*` means the RAM slot is newer than the card: progress
  that exists in memory and nowhere else.
- Choosing it with no card: `No card`. With a card: `Saving to card`, then a
  stall of a quarter of a second to two seconds depending on how many bytes
  differ, then `Game saved` or `Save failed`. The engine has no thread to
  animate a wait cursor from, so the message goes up *before* the stall to
  say what the stall is.
- **Load Game** does one `RALL` (~370 ms) when the list opens — the one moment
  worth a whole-card read, since one of the entries may be on the reader —
  and then lists `AUTOSAVE.LBA` and `CARD.LBA` as the engine always did.
- The strings (950/951/953, 960–964) come from a built-in table in
  `MSG_CUST.C`: they are not in `TEXT.HQR`, and before the table existed the
  two pause-menu buttons drew blank.

## 7. The card under MAME

Mounting: `-memc build/card_blank.bin`. The device declares
`is_creatable() == false`, so the file must exist and be exactly 120 bytes —
`tools/mkcard.py` makes one, with a UID if you want a specific one.

**Where the card actually is.** MAME loads the file given to `-memc` but
persists through `battery_save()`, into `nvram/hyprscan/<basename>.nv`. The
`.bin` stays exactly as `mkcard.py` wrote it however many saves the game
performs; **the `.nv` is the card.** Worse, on the next run `call_load()`
reads the `.bin` and then lets `battery_load()` overwrite it from the `.nv`,
so a "blank" card handed to a later run is not blank. To really start over,
delete the `.nv` too. `tools/cardinfo.py nvram/hyprscan/card_blank.nv`
decodes what is on it.

**MAME models absence, not time.** `is_loaded()` is checked on every
transition, so an unmounted card genuinely does not answer and "No card" is
reproducible. But its writes are instantaneous (§2), and it lets the program
write bytes a real tag protects (§1).

**MAME truncates `RALL`.** `m_resp_idx` in `hyperscan_card.h` is a `uint8_t`
counting half-bits of the answer; a `RALL` answer is 2,228 half-bits, so
after 256 it wraps and the tag starts its answer again. A reader clocking
the full response gets the first ~12 bytes over and over, parity still
checking out — which looks like a protocol bug in the reader. The port
detects it and falls back to 96 `READ`s; `mame/0003-hyperscan-card-resp-idx.patch`
is the one-line fix (`uint16_t`), optional because the game is correct
without it. Before: 1,260 ms per load, 7 fallbacks. After: ~380 ms, 0.

## 8. What is verified, and what is not

- `tests/save/test_save.c` — 35 host-side checks: pack, unpack, round-trip,
  the exception list, the refusal at eight. Build with a host gcc, not the
  score toolchain.
- `tools/m19_card.lua` — the driver in MAME: write, read back, verify; a
  second write of the same game programs 6 bytes, not 95.
- `tools/m19_save.lua` — the engine's own autosave succeeds ("Error Writing
  Saved Game" is gone).
- A manual save from the pause menu, read back with `cardinfo.py`.

Not verified, because there is no console: the pulse-width windows are
MAME's model of the reader, the programming time is a datasheet figure, and
the antenna, the card's position on the lid and what happens when it is
lifted mid-write are things only hardware can answer. The design assumes the
worst of all three — magic last, verify by re-read, refuse rather than
truncate — so that the first console test can only make it faster, not
wrong.

## 9. Not the NOR flash

The SDK has a complete driver for the console's NOR flash
(`NorFlash_SectorErase`, `NorFlash_WordWrite`), and it would hold a thousand
saves. **It holds the firmware the console boots from.** Erasing the wrong
sector renders a real HyperScan unbootable, with no recovery path, to obtain
what the card already provides. `NorFlash.c` is still in the Makefile's SDK
object list from the bring-up days; nothing in the port references it, and
nothing should.
