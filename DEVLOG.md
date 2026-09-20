# DEVLOG — what a 1994 DOS engine does on a 2006 toy console

Little Big Adventure was written for a 486 with VGA, a Sound Blaster and a
hard disk. The HyperScan has a 108 MHz S+core 7, an RGB565 TV encoder, a
DAC FIFO, a bare CD mechanism, and 96 writable bytes on a trading card. Every
entry below is something that was not obvious going in, was found by
measuring, and cost at least one session. They are in roughly the order the
port met them.

There is no physical HyperScan in this project. Everything was measured
under MAME's `hyprscan` driver, which is the reason for a recurring theme:
*what the emulator gets wrong, and how to build so it does not matter.*

---

## 1. The sixteen compiler bugs that were not

The homebrew scene documents sixteen codegen bugs in `score-elf-gcc`: the
fifth argument read from the wrong stack slot, `r3` used as a scratch
register in non-leaf functions, jump tables broken past five cases, small
copy loops skipping an iteration. Designing around them means no more than
four arguments, no non-leaf functions in hot paths, unrolling by hand —
which is to say, not porting a 28,000-line engine.

They are artefacts of the toolchain that found them: GCC 14 with the
`score` backend that upstream deleted in 2014, built **big-endian** and
post-processed with `objcopy --reverse-bytes=4`. The official Sunplus
toolchain — GCC 4.2.1, from the era the backend was in-tree and tested —
ships inside the SDK's IDE installer as an InstallShield cabinet. Extracted
and pointed at `tests/torture/torture.c`, which re-creates every one of the
sixteen shapes, the disassembly shows none of them: canonical `push! r3`
prologues, correct stack offsets, a bounds-checked jump table, and the small
copy loop compiled to the **hardware loop counter** (`mtsr sr0` + `bcnz`)
with both stores present.

The flag that matters is `-mel`. The console is little-endian; the ELF
comes out native, and there is no byte-swap step to get wrong.

## 2. The SDK never turns on interrupts

The SDK ships a vector table and a save/restore stub (`Sys_isr.s`) and
nothing, anywhere, writes `CP0_EXCPVEC` or `PSR.IE`. On a quickloaded image
the vector base is still `0x9f000000` from reset — unmapped — and the CPU
would not take an interrupt if one arrived. The CPU forms the target as
`(EXCPVEC & 0xffff0000) + 0x200 + (vec << 2)`; the table is linked at
`0xA00901FC`, so `EXCPVEC = 0xa0090000`. `src/irq.c` sets both, and
replaces `Sys_IRQ.c`/`User_IRQ.c` — a 64-way switch into forty empty
functions — with a table of pointers.

Vectors, from the MAME source since the manual does not list them for this
part: **39** I²C, **53** PPU vblank, **56** timers (all six share one line),
**60** CD servo, **63** SPU.

Turning them on revealed one thing at once: the I²C line **had been asserting
all along**, once per byte, and simply never taken. With `PSR.IE` high it
became ~1,700 spurious interrupts in ten seconds. The handler in `input.c`
exists to clear that flag, not to make the driver interrupt-driven.

## 3. The I²C driver that deadlocks on byte one

`HS_Controller_Init()` from the SDK never returns. Two bugs in `I2C.c`:

1. `I2C_Init()` never sets `C_I2C_INT_EN` (bit 1 of `P_I2C_INT_STATUS`), and
   the completion flag (bit 0) only rises when the enable is set —
   `spg290_i2c.cpp`: `if (m_irq_control & 0x02)`. `I2C_Read8()`'s
   `while (a == 0)` spins forever on the first byte.
2. The acknowledge is `*P_I2C_INT_STATUS = C_I2C_INT_FLAG;` — a bare store
   of `0x01` that clears the enable along with the flag. A fixed init would
   hang on the *second* byte instead.

The replacement holds the enable high, waits on **two** independent signals
(the interrupt flag *and* the mode register's ACK bit), and bounds every
wait with a spin budget, so a silent bus degrades to "no input" rather than
a lockup. Rebuilt with the enable forced low (`-DHS_I2C_NO_INT_EN`, ACK path
only) it reads every button and axis identically — the driver does not
depend on MAME's interrupt semantics, which is the guarantee that matters
with no hardware to check against.

The cost is real: **379 µs per byte, 1.9 ms for a five-byte read**, entirely
from the rate register. The SDK's value (0x258, 11 kHz) would cost 17 ms;
66 gives 100 kHz, I²C standard mode.

## 4. The time base has to be an interrupt

The engine does not want a wall clock. `TimerRef` is a counter, and
`RestoreTimer()` in `P_ANIM.C` *rewinds it backwards* after a menu, so the
platform may only ever do `TimerRef++` at 50 Hz — the same rule the DS port
arrived at. And the timer's overflow bit is a flag, not a count: with a full
redraw costing tens of milliseconds, a loop that samples it once a frame
loses whole ticks.

Choosing the clock: MAME models the source as 27 MHz / (N+1) with N in the
low byte of `P_TIMER_CLK_SEL`, **and runs a callback on every single tick** —
asking for the undivided 27 MHz means 27 million callbacks per emulated
second. `N = 215` gives exactly 125 kHz, and 125,000 / 2,500 is exactly 50
with no rounding (preload 63,036; the counter climbs to 0xFFFF). Measured
over 30 s: **50.017 Hz**, the residue being ±1 tick quantisation.

*Still open:* in one profiled run the tick fell to **39.3 Hz** during a heavy
frame. Any window with interrupts masked longer than 20 ms loses a tick, and
`TimerRef` is the time base of everything. Not yet understood.

## 5. Why the video is interlaced

`TV_Init()` selects `C_TV_INTERLACE_MODE`, so one vblank is a whole NTSC
frame of two fields: **29.97 Hz, not 59.94**. Progressive would double the
rate, but at QVGA MAME's non-interlaced path reads only the even rows of the
framebuffer and duplicates them — half the vertical resolution, gone. At
30 fps the vblank is above anything LBA will reach anyway.

The flip is one write to `P_TV_BUFFER_SEL`: the encoder holds three
framebuffer addresses and the register picks which one is scanned. Nothing
is copied.

## 6. The heap contained the framebuffers

`libgloss.c` takes its heap from `static unsigned char _heap[HEAPSIZE]`, an
8 MB array in `.bss`. That put the heap at `0xA009C948..0xA089DE88` — with
the framebuffers at `0xA0500000` **inside it** — and crt0 dutifully zeroing
8 MB on every boot. Nobody had noticed because nobody had called `malloc`
yet. The engine wants a couple of megabytes; this port parks ~8.7 MB of HQR
in RAM. It would have failed the moment it mattered, and in the most
confusing way available: a heap write that repaints the screen.

`src/memmap.h` fixes the layout and `src/syscalls.c` hands out the real
free space between the end of `.bss` and a ceiling under the framebuffers.
`.bss` went from 8.4 MB to 12 KB, ~14.5 MB of heap remains, and one
allocation too many is now a `NULL` from `malloc`.

## 7. Seeks are the currency, and MAME gives them away

Every `cd_read()` writes the servo's control register, and in the servo
model *any* write there recomputes the target and repositions: **a read is a
seek**. MAME charges nothing for it — `m_cur_sector = LEADIN + m_seek_lba;
// TODO: seek time` — which makes it exactly the wrong instrument for
noticing. The first `fs.c` split every request into head/body/tail and
verified all 8.7 MB in 12 emulated seconds while issuing **about nine
thousand seeks**. On a real mechanism at 100–200 ms each that is twenty
minutes of head travel to load the game.

A 64-sector (128 KB) read-ahead, with a direct path into the caller's
buffer for large aligned requests, took the same load to **78 seeks for
4,442 sectors**. Under MAME the change costs a couple of seconds *more* (an
extra `memcpy`, and seeks are free): the emulator values the trade-off in
exactly the opposite direction from the hardware.

## 8. `LBA.CFG` must be CRLF

The first boot stopped with no fault and a black screen. `DEF_FILE.C`'s
`ReadThisString()` scans a value until it sees **CR (13)** and never tests
for LF; on a Unix-terminated file the value swallows the rest of the file,
`"NoMidi"` stops matching, the engine concludes a MIDI driver was requested,
`InitMidiDLL()` fails and calls `exit(1)`. Silently — the symptom was the CPU
spinning inside our `_exit`, found by sampling the PC from Lua.

Found so quickly only because of the **console in RAM**: the engine narrates
its own startup with `printf`, but the UART goes nowhere (MAME leaves the
port unmapped; real hardware has nothing attached). `_putc_r` mirrors
everything into a ring buffer at `TRACE_BASE + 0x1000`, and the harness
drains it as text. Fourteen lines of Lua, and — see §20 — it kept paying
for itself.

## 9. Two packing traps, and the second only shows after the first

**`#pragma pack` does not exist on this target.** GCC 4.2.1's score backend
prints `#pragma pack(push[, id], <n>) is not supported on this target` and
lays the struct out with natural alignment anyway. That is a plain warning,
and the `-w` any 1994 codebase needs hides it entirely. `T_HEADER` in
`HQ_RESS.C` is 10 bytes on disk and became 12, so every `Load_HQR()` left
the file **two bytes past** the start of the compressed stream. LZSS still
decompressed something recognisable — the right image, rotated horizontally
by ~310 bytes and a third wrong — which reads as a blitter or palette bug.
Found by dumping `Log` from MAME and comparing it byte by byte with the same
entry decompressed on the host: the constant 310-byte shift cleared the
blitter, the tail of the buffer showed the offset of two, and `ldiu! r6, 12`
in the disassembly of `Load_HQR` confirmed it. `__attribute__((packed))`
works; `PORT_PACKED` went on all eleven on-disk structs.

**Then: Watcom packed at `-zp2`, not to the byte.** `packed` closes *every*
gap, but the on-disk layouts have gaps in them: members are aligned to 2,
so an odd-sized member followed by a 2- or 4-byte one has a pad the file
actually contains. `T_HEADER` is unaffected — all its members are naturally
aligned — which is why byte-packing looked like the whole answer. The FLA
header is not: it declares `char Version[5]` followed by a `ULONG`, and in
the file the version string takes **six** bytes. Byte-packed, the animation
cadence read 0 and `50 / ImageCadence` divided by zero — which on this CPU
is `EXCEPTION_CEE`, a halted machine, not a silent zero. The pads are now
written into the structs explicitly, which is correct on any compiler and
can be checked against a file. *Validate any on-disk struct by walking a
real file with a throwaway Python reader before believing the declaration.*

## 10. The palette is a DAC

LBA's fades (`FadeToPal`, `FadeToBlack`, `WhiteFade`) do `Vsync();
Palette(workpal);` in a loop and never touch a pixel. On VGA the DAC sat
downstream of the framebuffer, so rewriting the palette re-coloured what was
already on screen for free. Here the palette is applied at conversion time,
and without a repaint every fade ended with the screen still coloured by the
palette it was drawn under — after a `FadeToBlack`, black. Hence the
invisible menu backdrop: only the rectangles the menu redrew afterwards ever
appeared.

The repaint **cannot** be a reconversion of `Log`, because `Log` holds more
than the engine shows: `DoFire()` paints its 50 rows across all 640 pixels
and only the 550-wide menu box is pushed to the screen, so reconverting
`Log` leaks bands that were never meant to be seen. `src/video.c` keeps an
8bpp shadow of what was *presented* (75 KB) and a palette write re-expands
it — exactly what the DAC did, as a flat lookup. Palette writes only mark;
the repaint happens at the next `Vsync`, because `SetBlackPal()` alone calls
`PalOne()` 256 times.

## 11. `Phys` is not the screen, and `Log` has two readings

On DOS `Phys` was the VGA aperture at `0xA0000`: writing to it *was*
displaying. Here it is an ordinary 64,000-byte buffer, so everything the
MCGA paths compose into it — the menu backdrop, the logo — shows nothing
until the platform presents it explicitly. `Mcga_Flip()` is the only present
in the FLA loop, which never passes through `Vsync()` or `Flip()`.

And `Log` is read two ways. The F12 zoom leaves the game rendering into
`Log` as a 640×480 page and crops a 320×200 window. The FLA player instead
overwrites the head of the same memory with a **contiguous** 320×200 image:
both codecs in `ADFLI_A.C` advance by the `deltax` that `PLAYFLA.C` passes,
always 320. So the flat `memcpy(Phys, Log, 64000)` in `Mcga_Flip()` was
right — I "fixed" it into a strided copy and the movie arrived as its top
half on black, every output row skipping a source row. What *was* wrong
were `BlackFrame()` and `CopyFrame()`, translated with the doubled stride.

## 12. The ISR stub did not save the loop counter

From the menu there was no way forward: `Fire` froze on entering
`DoGameMenu()`. On DOS `Key`/`Joy`/`Fire` were maintained by the keyboard
IRQ, **asynchronously**; `DoGameMenu()` has its `Vsync()` commented out and
sits in `while (Joy OR Fire OR Key)` after each selection waiting for the
release. With polling hooked to `Vsync()`/`Flip()` the intro responded and
the menu was inoperable. The poll moved into the vblank interrupt, as on the
DS. And there a much bigger problem came out.

`Sys_isr.s`'s `save_reg` spills `r1`–`r31` plus CR and EPC. It does not save
**CEH/CEL** (the `mul`/`div` result pair, live until the `mfce` that reads
it) nor **`sr0`, the hardware loop counter** — the one GCC 4.2.1 uses for
small copy loops (§1). While the handlers were a register write and a `++`
it cost nothing. The moment one does real work, it rewrites the interrupted
code's trip count: **a `memcpy` that runs too long is a corruption with no
bad pointer anywhere to find.**

The symptom: the first kilobytes of the image at `0xA00901FC` overwritten,
so every exception vectored to `0xFFFFFFFF`, which is `EXCEPTION_RI` —
raised *on the vector itself*, in a loop. PC pinned on `general_vec`,
`TimerRef` stopped, `intmsg()` never reached, so not even the cause
recorded. A MAME write tap on the vector table saw **no** writes, which is
exactly the signature: there is no culprit storing there, there is a loop
that was told to run 2³² times. Fixed by saving `mfcehl`/`mtcehl` and
`sr0`–`sr2` in the frame's spare slots.

The same file had a second defect: the restore path reloaded `r30`/`r31`
from the general slots and then overwrote them with CR/EPC on the way to
`mtcr`, handing the interrupted code two registers full of coprocessor
state. Harmless only because GCC treats `r30`/`r31` (K0/K1) as reserved.

## 13. Three things that were not what they looked like

**Both stick axes are reversed.** MAME declares the stick `IPT_AD_STICK_X/Y
… PORT_REVERSE` and passes the port value straight into the report: up and
right are the values *above* centre. The m5 axis test compared against the
value injected into the port, so it proved the bytes crossed the bus intact
and could not notice.

**The frame regulator is not a speed control.** The game seemed to run at
triple speed and the obvious theory was the 50 Hz regulator in `PERSO.C`'s
`MainLoop`, which the earlier ports re-enabled. Built at 1 tick and at 3
and profiled: the repaint rate follows the constant exactly (29,100 px/s
against 14,094) and **nothing on screen moves at a different speed**. LBA's
motion is driven by `TimerRef`, not by the frame count; the regulator only
buys smoothness. The perceived speed was MAME running at 690 % with
`-nothrottle`.

**The trail behind Twinsen was a rounding.** `video_blit_log()` mapped the
dirty rectangle by truncating **both** edges. At 2:1 a box ending on an even
source column gives `(x1+1)/2 == x1/2`, so the destination pixel that column
lands in stays outside the loop bound and is never rewritten. The top edge
rounds **up**, as the comment said and the code did not.

## 14. Sound: the only first-party code that drives the DAC

The chip has two ways to make sound and only one is attested by shipped
code. The 24 hardware channels decode ADPCM themselves, but want their own
format in a 22-bit *word* address window (the first 8 MB only), LBA's
samples are 8-bit VOC scattered over a heap that reaches 14 MB, and MAME's
model of those channels was inferred from one retail game with undocumented
mode bits: three assumptions stacked.

The DAC FIFO is driven by `PPCSDK/…/USBLoader/MP3Drv/MP3Drv.c`, which is code
that shipped. Every value in `src/audio.c` is copied from it, not deduced:
`27000000/rate - 1` into `P_DAC_SAMPLE_CLK`, `0x4000|0x0004|0x0003` to arm,
`0xC000|…` to acknowledge in the ISR. Its *Left_Only* path writes the same
value into `FIFOArray[i*2]` and `[i*2+1]` — that is what fixes the format,
interleaved L,R frames — and the samples are **unsigned** (`^= 0x8000`), so
a zeroed buffer is full-scale DC, not silence.

**The MAME patch had to be aligned before the driver was written.** The
model treated the FIFO as mono and compensated by running the clock at
54 MHz — consuming the right number of words per second while reading
L,R,L,R as double-rate mono. `spg290_spu.cpp` now decodes bits 1:0 (size)
and 2 (stereo) of `P_DAC_INT_STATUS`, the clock is the 27 MHz crystal, and
the timer pours half a buffer at a time instead of one sample per tick.

The mixer is the DOS driver's semantics (`WAVE.C` + `WAVE_A.ASM`), by way of
the DS port's SDL shim where they had already been worked out **with real
voices** — which is where the separate `idx`+`frac` cursor comes from: a
narration exceeds 65,536 samples, a 16.16 cursor wraps, the sample loops,
`WaveInList` never ends and neither does `Dial()`.

Something the format gives away: byte 0 of a sample buffer is not VOC. It is
`'C'` in `SAMPLES.HQR` and 0/1 in the VOX banks, where `MESSAGE.C` reads it
as `FlagNextVoc`; the DOS driver reused it as the interpolation flag
(`byte0+1 < 10`), so voices are filtered and effects are not, without anyone
having decided so.

The mix runs **in the interrupt**: half a buffer is 1,024 frames, 46 ms, and
a full LBA redraw costs more, so the main loop cannot guarantee the refill.
That is only legal because of §12.

## 15. The CD ring was one sector deep

The ISR mixer exposed a defect that was already there. `cd_read()` had a
ring of **one frame**: copying each sector before the next arrived was a
real-time constraint, and at 8× a sector lasts 166 µs against milliseconds
for a fill. The first `audio.c` stepped aside while `cd_streaming` was high
— which during a movie, where the disc streams continuously, means repeating
the same 46 ms at ~5 Hz. An *audible* stutter.

The fix is in `cd.c`: a **64-frame** ring, and instead of the
`DSP_FRAME_FOUND` flag, the servo's write pointer. A flag only says "at least
one frame arrived", so as soon as the CPU is more than a sector behind the
count stops matching and the same frame is copied again while the disc
moves on; a *position* can be read late and is still right. And there is an
overrun counter, because a lost sector must never again be silent.

## 16. 8× was never a requirement

The number in `DSP_SPEED` was 8× for one reason: it measured better. What
the movies actually ask, from the FLA headers against file size, knowing the
player waits `50/ImageCadence` whole ticks per frame:

| | KB/s | × CD |
|---|---|---|
| DRAGON3 (the most demanding) | 384 | 2.56 |
| SURF / NAVETTE | 326 | 2.18 |
| INTROD (15.1 MB, 73.9 s) | 210 | 1.40 |
| median of the 23 | ~155 | ~1.0 |

Eighteen of twenty-three are under 2×. How fast the mechanism *is* has no
first-hand source; the only direct claim found is a forum post — 4×, *"when
40-50× units were commonplace in 2006"* — weak, but more than 8× ever had,
and it is what the driver uses now. Follow it through: if the drive is 4×,
600 KB/s, then the HyperScan's legendary loading times **are not bandwidth**.
What is left is seek — which is what §7 measured on its own, nine thousand
against seventy-eight. The reputation belongs to the head, not the data
rate.

## 17. Prefetch wanted the servo's interrupt

`cd_read()` armed the servo and then **waited** for every sector: at 4× a
128 KB block is 213 ms during which the decoder does not decode. Leaving the
servo running into the ring while the CPU works is obviously the right
shape, and for all of m12 it was impossible.

`poll_servo()` inferred arrivals from the *movement* of the write pointer
between two looks, so it could only measure absences shorter than one ring
revolution. With the servo free, the game decides the absences — pauses of
**four seconds** between two reads were measured during the intro, against
1.7 s of ring at 4×. The pointer laps, `produced` loses whole revolutions,
the overrun test *passes*, and the caller receives sectors from elsewhere on
the disc with the counter reporting zero. Corrupt video and audio, and
diagnostics swearing all is well.

There is no sector-count register to bound the run. The bound has to come
from something that runs whether the game is looking or not, and the only
such thing is **the servo interrupt, vector 60**: it parks the mechanism at
the high threshold, and the reader restarts it at the low one. Ring 512
frames, park at 448, resume at 256, margin 64 = 213 ms at 4×. The margin is
**also the detector**: if the pointer moved more than that between two
looks, interrupts were masked long enough for the servo to run uncounted,
and the same arithmetic notices — a legitimate step is one sector, that is
not. Then it re-seeks instead of copying.

The handler **does not count interrupts**: it calls the reader's own
routine, which reads the pointer. An edge count survives neither a loss nor
a duplicate; a position read late is still right. And MAME confirms it from
the other side: `cd_servo_irqs` is sometimes *less* than sectors delivered,
because the core does `m_pending_interrupt |= 1 << n` and two close sectors
raise one interrupt.

A second effect, unplanned: the DSP port is three registers used as a
transaction (address, data, exec) and the ISR performs its own acknowledge
through it. `dsp_read`/`dsp_write` now mask interrupts, or an interrupt
between the address and the exec redirects the request the interrupted code
was making.

A/B with `-DHS_CD_NO_PREFETCH`, same run: intro movie **20 s vs 32 s**, 9.6
vs 6.1 presents/s, stalls **0 vs ~2,900**. What prefetch costs is sectors,
not time — 47 % speculative over a session, all of it at load points where
the next request is a different file. During a movie, a sequential run
through one file, it does not miss a prediction.

## 18. Voices: zero new lines

The mixer already covered them; the work was data. The VOX banks are one
archive per island plus `GAM`: an offset table, then per voice
`size`/`sizelzss`/`method`. **All voices are LZSS** — `method` is 1
everywhere, the raw branch of `PlaySpeakVoc()` is never taken. Verified on
the host before compiling, by re-implementing the LZSS in Python on
`EN_000.VOX`: the expansion is exactly the declared size and begins with
`00 "reative Voice File"` — the `'C'` of the VOC magic overwritten by
`FlagNextVoc`, §14 confirmed by the data. **11,111 Hz, 8-bit, mono.**

`FlagSpeak` was *already* TRUE: `InitLanguage()` raises it when `LBA.CFG`
names a `LanguageCD` and the wave driver answers, both true since m12. The
engine had been looking for voices ever since and not finding them. There
is no `EN_SYS` or `EN_CRE` — those two dialogue files were never voiced, and
`MESSAGE.C` handles it by leaving `FdNar` null.

`tools/m14_voice.lua` reports the four points where the path can break,
because from outside they are all the same silence: `FlagSpeak`, `FdNar`
(archive found), `MaxVoice` (archive *parsed*), and `voices` — `WavePlay`s
accepted on the `SPEAK_SAMPLE` handle, which needs its own counter because
`wave_plays` keeps rising on sound effects regardless.

## 19. The micro-freeze before every new sound

As seen from the couch: press jump, the wind-up is instant, then a few
tenths of a second of freeze, then the jump with its "boing". For every
sound effect **never heard before**; the second time, no. So the freeze is a
*fetch*, before the sound and not in it.

Two sessions had assumed the fetch was `SAMPLES.HQR`, and preloading the
whole archive had not removed it. No counter could settle it: they were all
per subsystem, and the question is **per file**. `src/fs.c` now charges every
read to the directory entry that asked, in sectors and in 50 Hz ticks spent
*inside* `fs_read` — frames the game did not draw. First 13 s, touching
nothing: `SAMPLES.HQR`, 4 opens, 512 sectors, **1,720 ms lost**. Four opens
are two misses: **256 sectors, half a megabyte and ~860 ms for a 5 KB
sample**.

Two defects multiplied. **`Size_HQR()` was dead work**: it opens the archive,
walks the offset table, finds the entry and reads its header to return
`SizeFile` — and both `HQR_Get` and `HQR_GetSample` immediately repeat the
identical walk and overwrite `size` with the same value. On DOS a wasted
read against a disk cache; here a real trip to the CD. Two lines deleted.
**The read-ahead did not look at the request**: `ra_fill` pulled 64 sectors
for any read, including the 4-byte ones of the offset table. Four out-of-
buffer seeks × 128 KB = the 256 sectors measured. It now sizes itself to the
request; the fixed block dated from when `cd_read()` re-sought on every call,
and since §17 the servo stays on and resumes where it was.

After: 4–11 sectors per miss, lost ticks from 86 to 0–3 per 10 s window,
sounds start instantly. And it explains why the preload failed: its 55,000
sectors for 243 entries are **226 per entry** — exactly this tax multiplied
by the archive. It was not badly implemented; it was measuring the defect
without our recognising it.

## 20. The handle leak that was three bugs

With fast loads the game got further than before and showed more:
invisible doors, missing upper layers, a dialogue that hangs. The first
theory — `HQ_Mem` exhausted — **was wrong, and the measurement said so**:
265/390 KB.

The real cause: `CloseFdNar()` has its body entirely commented out, and it
was not the port that did it — it arrives that way from the published
Adeline source, and the DS port has the identical file. `InitFileNar()`
calls it deliberately to close the voice archive before opening the next.
Never closing, it leaks a handle per island change. Invisible on DOS;
`src/fs.c` keeps **eight**, and at 171 s of normal play they were gone. From
there `fs_open` refuses *every* file: doors stop being drawn, layers vanish,
and at the first line `Dial()` waits forever for a voice that can no longer
be read. A second handle leaked in `HQRM_Load`, which left through three
error branches without `Close()`.

Re-enabled and closed: doors visible, **holomap working — including the full
3D planet view**, 324 s without a fault. The three open problems were one
problem. The lesson is the one from §8: the engine could already say it.
`MALLOC.C` and `FICHE.C` print their reason on `printf`, and no harness had
been reading that console. `drain()` in `m18_fetch.lua` made `Not Enough
Memory: Body.HQR in HQ_Mem` appear at the first opportunity.

## 21. `Malloc(-1)` is a question

`PERSO.C` sizes the sprite, sample and animation caches from `Malloc(-1)`,
which on DOS answered "how much memory is free". The port answered 0, so
every pool fell to its floor — 50 KB sprites, 200 KB samples, 100 KB
animations — on a console with about twelve megabytes going spare. A cache
at a twentieth of its intended size does not degrade gracefully: it evicts
something still in use, and the game either re-reads it off the CD (the
hitch on every new sound) or carries on without it (an animation that never
plays).

## 22. Ninety-six bytes

The console's only writable storage is the RFID card — a **Type 1 (Topaz)**
tag, 120 bytes in MAME's model, of which the user area is 96 at offset
0x08. The console's NOR flash has a full driver in the SDK
(`NorFlash_SectorErase`, `NorFlash_WordWrite`) and **must not be used**: it
holds the firmware the console boots from, and erasing the wrong sector
turns a real HyperScan into a brick, without recovery, to get what the card
already does.

Two asymmetries decide the design. **`RALL` reads everything in one
transaction**; **`WRITE-E` writes one byte per transaction**, so a save is
~92 transactions and about a second of continuous contact on real hardware.
Hence: every scene change → RAM only, a serialised state always current
(free, cannot fail, needs no card — and covers what autosave is for in LBA,
dying and continuing). Manual save → the card, synchronously, the player
having **placed the card first**, so no modal "waiting for card" loop; the
menu entry shows whether a card is detected (one `REQA`, instant) and a `*`
when the RAM slot is newer than the card. Load → `RALL`, one transaction.

The format: the engine's own 468-byte autosave stream, packed. `TabHoloPos`
compresses losslessly (HOLOMAP.C writes only 0, 64 and 129, and the one
read of bit 64 is commented out). `ListFlagGame` **does not**:
`LM_SET_FLAG_GAME` copies an arbitrary byte out of a life script, so the
bitmap says "non-zero" and flags holding anything else go to a **7-slot
exception list**. An eighth refuses the save, not truncates it, and
`save_flag_exceptions` keeps the high-water mark — it is by playing that one
finds out how many are needed. Validity byte written **last**, so an
interrupted write cannot look valid.

The API was written against the hardware's constraint from the start —
`card_present()`, `card_write_begin()`/`card_write_step()`, never a
`card_write()` that returns success — so that no code can be written that
works only because the emulator's write is instantaneous. MAME's *is*
instantaneous; a real tag wants milliseconds of EEPROM programming per
byte. That delay lives in `card.c` (`T_PROGRAM_US`), deliberately: it belongs
in the code that runs on the console, where it is part of what the player
pays, not in the emulator, where it would be a number nobody ships. Same
trap as the 8× in §16.

## 23. The card wire

`src/card.c` bit-bangs the protocol on **pin 1 of the CSI GPIO**
(`P_CSI_GPIO_SETUP` out, `P_CSI_GPIO_INPUT` in). Only the **high pulse
width** carries information, in cycles of the 13.56 MHz carrier: <500 is a
0, 500–999 a 1, 1000–4999 a read strobe. Low time is free, so interrupts are
disabled around a single pulse, not around the transaction. The delay loop is
**calibrated at runtime** against the 50 Hz tick, the only clock that is
correct in the emulator and on the console alike.

The "card detect" register MAME exposes at `0x08200070` is **not used**:
that address is `P_IOB_GPIO_INPUT` on real silicon, and the MAME source
labels it `homebrew extension`. `card_present()` is a real `REQA`, ~1 ms.

Two things about MAME's card. `m_resp_idx` is a `uint8_t`, and a `RALL`
answer is 1,114 bits: after 128 it starts over, and a reader clocking the
full response gets the first ~12 bytes again and again with the parity still
checking out — which looks like a protocol bug in the reader. The driver
detects it and falls back to 96 single `READ`s (1.26 s instead of 0.38);
`mame/0003` is the one-line fix. And MAME does not write the `.bin` given to
`-memc` back: the card persists in `nvram/hyprscan/*.nv`, and on the next
load that file overrides the `.bin`. Starting from a blank card means
deleting both.

## 24. `F_CTRL` does two things, and the second is the one that counts

The pad has no d-pad and no room for the DOS keyboard: four coloured
buttons, Start, Select, two shoulders, two triggers, a stick. The final map
is the keyboard laid over the pad one key per button (README, *Controls*);
the interesting part is behaviour switching.

`F_CTRL` is the **held modifier** that opens the behaviour menu:
`MenuComportement()` is `while (Fire & F_CTRL)` reading `Joy` left/right
*inside itself*. The only linear cycle that function can express is the one
the player drives by holding a key. But that menu is also **the game's only
status screen** — `DrawMenuComportement()` ends in `DrawInfoMenu()`: life,
magic, coins, keys. There is nowhere else to read them. So red stays
`F_CTRL`, even though the shoulders cover the switch without it.

The shoulders call **`SetComportement()`** directly — the whole switch, the
same call the menu makes on exit and the `SET_COMPORTEMENT` life-script
opcode makes. They post to a **mailbox** (`PORT_ComportementStep`), not to a
bit in `Fire`: everything else in the map is level state the engine re-reads
while the key is down, but a behaviour change is one shot, and
`SetComportement()` reloads Twinsen's body, which cannot be done from inside
an interrupt. Consumed in `PERSO.C`, once per `MainLoop` iteration — the
same shape as the DS port's touch mailboxes. Verified: four R1 and four L1
presses, the ring closing both ways, **zero** frames with `Island == 255`
(the behaviour menu, and only it).

Under MAME, Esc is the emulator's quit key, so `UI_BACK`/`UI_CANCEL` move to
Backspace — the only plain key that neither the game nor MAME's defaults
use, Ctrl, Alt and Shift all being taken.

## 25. A frame rate, at last

Until m21 there was no frame-rate measurement at all: `CmptFrame` does not
behave as a counter when read from outside. Two instruments. `PORT_frames`
is a real `MainLoop` count, and `PORT_frames_idle` is the half that matters:
the regulator **waits** for its 50 Hz tick before the next frame, so a frame
with time to spare and a frame that overran look identical in a frame
count. `idle/frames` is the headroom, directly.

And a sampling profiler (`tools/m21_profile.lua`): every video frame, read
the PC and charge it to the function containing it. Statistical, but it
touches no hot code, so it cannot distort what it measures. Two traps, each
costing a run: GCC emits its `.L*` branch targets as text symbols, so every
hot loop was charged to `.L69` (`fsyms.lua` drops them); and a sample inside
`memcpy` is not a finding, it is a question — `r3` is the S+core link
register, libc's leaves never touch it, so sampling it gives **the caller**.
From then on the profile says `memcpy <- AffObjetIso`, which is information.

The first profile, one actor, 38.7 fps, 0 % headroom: `memcpy` 19.2 %, `RW`
7.0 %. `RW()` reads a WORD from model data and was `memcpy(&v, p, 2)`, the
normal unaligned-read idiom. score-elf-gcc at `-Os` does not fold it:

```
RW:  push r3 / addi r0,-16 / jl memcpy / lhu r4,[r0,12] / extsh / br r3
```

A stack frame and two nested calls into libc to move two bytes — per field,
per vertex, per polygon, per actor. Same story in `SergeSort`, where
`tmp = *a; *a = *b; *b = tmp;` on an 8-byte struct became **three** `memcpy`
calls in the inner loop of the polygon depth sort.

| | fps | headroom |
|---|---|---|
| start | 38.73 | 0.0 % |
| `RW`/`WW` by byte | 46.40 | 0.4 % |
| small copies unrolled | 46.80 | 0.6 % |
| `SergeSort` swap by field | **49.09** | **13.5 %** |

+27 % in one file, and the cell now hits the regulator's ceiling instead of
the machine's.

## 26. Geometry-bound, not fill-bound

`tools/m22_scenes.lua` writes `NewCube` — the same request a trigger zone
makes — so any scene on the disc can be visited and measured without
playing to it. Regression over 14 scenes: **18.8 ms a frame + 14.5 ms per
object**. And screen area predicts nothing: scene 32, 16,177 px of object
boxes, 22.1 fps; scene 70, 25,431 px, 20.6; scene 1, **6,452 px**, 15.1 —
a quarter of the area and slower than both.

| | share | scales with |
|---|---|---|
| `AffObjetIso` (transform, model decode, sort) | 27.7 % | geometry |
| `ComputePoly_A` (clip + edge walk) | 25.1 % | scanline height |
| `SVGAPolyGouraud` + `FillVertic_A` (fill) | 10.7 % | **area** |
| `video_blit_log` + `CopyBlock` (present) | 5.5 % | area |

The plan had assumed the countermeasure was fewer pixels — decimate at the
source, 4× off the rasteriser. The data says that would recover **~23 % of
the frame**, not 4×, at the cost of re-mastering the art and touching every
coordinate path. The levers are in the per-vertex and per-polygon work,
where 53 % of the frame is: LOD for distant actors (the only thing that
reduces geometry rather than pixels), and culling before the sort instead
of after it — `SergeSort` currently orders polygons that will be discarded.

## 27. "Are we really at 640×480?" — yes, and three pixels in four are thrown away

A fair question, because the screen showed everything decimated. Settled by
dumping the real buffer: `Log = A025DFF0, Screen_X=640 Screen_Y=480`, and
the 640×480 image pulled from RAM has Twinsen's face, the stripes on the
shirt and the brick texture **all legible** — none of which exists in the
320×240 output. Rasterisation was being paid at full resolution and a
quarter of it shown.

The TV encoder does 640×480 (`case 0x4: // VGA` in `tve_control_w`). Measured
rather than estimated: in game it costs **6.7 %** (49.09 → 45.81 fps),
because LBA's frame is incremental and the dirty rectangles are small. But
full-screen presents — movies, menus, fades — pay the 4× in full: the intro
nearly doubled. With one 600 KB framebuffer there is room for exactly one,
which is fine: presentation is single-buffered by choice.

**The caveat MAME cannot show.** 640×480 on NTSC is **480i**, and LBA's brick
texture is exactly the high-frequency content that flickers on an
interlaced set. The HyperScan outputs composite, whose luminance bandwidth
cuts horizontal detail around 300 pixels. On a real TV the gain is plausibly
much smaller than it looks here and might be *worse*. Without a console to
check, 640×480 is the default and `-DHS_VIDEO_QVGA` is the way back.

Three things followed from the choice: a dedicated 1:1 path in the blit
(the inner loop had been doing a 16.16 accumulator per pixel to reproduce
`s[dxx]` at a scale of 1 — 45.81 → 47.15 fps); a row cache in the scaled
path, because a 320×200 `Phys` stretched to 640×480 repeats 280 of its 480
destination rows exactly (intro −19 %); and `SwapTri`, which at `-Os` had
stayed a real function at 0.8 % of the profile for twelve loads and stores.

## 28. The Makefile did not track headers

Found trying the 640: editing `video.h` recompiled only the `.c` files
touched by hand, and everything else kept the old macro values. A build with
half its translation units at `VIDEO_W 640` and half at 320, linked, and
running. No diagnostic: it looked like a rendering bug. `-MMD -MP` and an
`-include` of the `.d` files.

## 29. newlib word-copies only if *both* pointers are already aligned

Disassembled, `memcpy` tests `(src | dst) & 3` and takes the unrolled word
loop only if source and destination are **already** 4-aligned; anything
else is a pure byte loop. Which is the normal case here, and the expensive
one: `CopyBlock()` restores the background under `ClsBoxes`, one call per
row of every dirty rectangle, source and destination being the same column
of two 640-wide buffers — they **always** share an alignment phase and
almost never start aligned, so three rows in four went byte by byte.
`AffGraph()` calls `memcpy`/`memset` once per RLE run, 1 to 64 bytes at a
time, where the call is most of the cost.

`translate/fastmem.h` adds `FastCopy`/`FastFill`. The key is that `FastCopy`
takes the word path whenever the two pointers **share a phase**, not only
when that phase is zero: align the head by hand, then move words. The
non-regression check is the one that matters when rewriting fills inside
rasterisers: a dump of `Log` at 640×480 compared pixel for pixel with the
one from before. 1,342 pixels of 307,200 differ, all inside a 31×87 box —
Twinsen's bounding box, at a different animation frame because the scene is
reached at a different instant. Bricks, beams, lamps, bed: identical.

## 30. The menu and the intro were a different machine

Reported from play: in game it is fine, "every single menu is torture".
Profiled separately from boot with nothing pressed. The natural objection is
that logos and menus are static and therefore the ideal case for the dirty
rectangle — **true, and the dirty rectangle works**: 6.46 screen rebuilds a
second. The cost is in two things it cannot cover by construction:

| | share |
|---|---|
| `video_repaint` | **29.4 %** |
| `ComputeFire` + `DoFire` | 27.0 % |
| `video_blit_log` + its memcpy | 21.3 % |

`video_repaint` is the fades (§10): `FadeToPal()` is `Vsync(); FadePal(…)`
in a loop, **51 steps**, touching no pixel — on DOS the DAC re-coloured the
screen for free, here every step re-expands 307,200 pixels. The boot goes
through about fifty of them. `ComputeFire` is the animated fire behind the
selected menu item — 15,360 eight-neighbour sums per frame on a fixed
320×50 buffer, costing the same before 640×480.

Done: `video_repaint` rewritten with 32-bit stores, unrolled. **Fades
removed** — `FadeToPal`/`FadeToBlack` jump to the end state; nothing depends
on the intermediate palettes, that loop reads no input and changes no other
state (`-DHS_KEEP_FADES` puts them back, and is also the right knob if on
real hardware the answer turns out to be "fewer steps" rather than "none").
The EA logo and the three narrated slides that open a new game cut, each
behind its own flag; the dream movie that follows stays. Boot to first
playable scene: **395 s → 205 s**, gameplay unchanged — and correct by
construction, since gameplay passes through none of these paths.

## 31. Dynamic resolution: follow the source, not the situation

The resolution is **two bits** in `P_TV_MODE_CTRL`, one write. The trap:
`C_TV_QVGA_MODE` is `0x00000000`, so the obvious `*P_TV_MODE_CTRL |=
C_TV_QVGA_MODE` is a no-op that leaves the VGA bit set. `TV_Init()` does
not fall into it only because it *assigns* the whole register.

The policy is not "menus at 320, game at 640" but "the output follows the
source". The FLA movies and the MCGA menu backdrop are a **320×200 `Phys`**
— scanning them at 640×480 costs 4× the present and cannot show one more
pixel, because there is none. The logos are 640×480 sources (the `RESS.HQR`
entries decode to exactly 640×480 bytes) and the main menu draws into `Log`
at 640×480, text included: at 320×240 those would lose real detail. So the
switch hangs off `McgaMode`, which the platform already tracks. No
heuristic, no hidden quality choice. Framebuffer and shadow are cleared on
the change, because their stride changes meaning.

Boot to first scene: **128 s**, from 205. Gameplay 48.0 fps, `presented
2965 px` per frame confirming the present is still 1:1.

**The serious caveat MAME cannot show.** An NTSC set resynchronises when the
mode changes: every 480i↔240p switch is plausibly a black flash or a roll of
up to a second, at the start and end of every movie. MAME reconfigures the
screen instantly. `-DHS_VIDEO_FIXED_RES` pins the output and forgoes the
gain, if on a real console it proves intolerable.

## 32. Still open

- **`TimerRef` loses ticks under load** (§4). Every speed measurement rests
  on it.
- **`HQ_Mem` reached 84 %** of its 390 KB in one long session and in an
  earlier one actually ran out. The scene/body split the probe should print
  does not work (`NbBodys` reads 0 at every peak), so the pool is not
  understood. To redo before raising a number from 1994.
- **Music.** The CD version's soundtrack is CD audio; nothing here plays it.
- **Nothing has run on hardware.** Every "the emulator cannot show this" in
  this file — interlace flicker, composite bandwidth, mode-change resync,
  EEPROM write time, the real drive speed — is a question for the first
  console.

## What generalises

- **Copy the shipped driver, do not deduce the register.** `MP3Drv.c` for the
  DAC, the SDK's own Makefile for the flags, the FLA file for the struct.
  Every place the port trusted a document or a model over shipped code or
  real data, it paid.
- **A position survives being read late; a count does not.** The CD ring,
  the servo interrupt, the card protocol — every time a flag or an edge
  count was replaced with a pointer, a class of bug vanished.
- **The emulator's free things are the hardware's expensive things.** Seeks,
  EEPROM writes, mode changes. Put the cost in the code that ships, never in
  the emulator, and keep an A/B switch so the gain is separable.
- **Read the console.** The engine says what is wrong on `printf`. Twice the
  answer was there before the investigation started.
- **Measure before optimising, then measure the thing you did not change.**
  The frame regulator, the preload, the pixel-count theory: each was a
  confident explanation that a counter refuted in an afternoon.
