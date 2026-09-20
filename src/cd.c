/*
 * cd.c — SPG290 CD servo driver.
 *
 * There is no CD read driver anywhere in the PPCSDK: its examples only *boot*
 * from CD and then do file I/O over USB (FatFS). This driver is written
 * against the register/DSP protocol implemented by MAME's spg290_cdservo HLE,
 * which Sandro Ronco derived from observing retail HyperScan titles.
 *
 * Model: the servo streams whole RAW frames (2352 bytes) into a ring buffer in
 * SDRAM all by itself, advancing the LBA after each frame, and raises a
 * "frame found" flag (DSP register 0x307) per sector. Software programs a seek
 * point in absolute BCD MSF, arms the servo, then consumes frames as they land.
 *
 * Because delivery is sequential and self-advancing, one seek followed by a
 * long sequential read is by far the cheapest access pattern — which is why
 * the CD layout puts each scene's data in one contiguous run.
 */

#include "cd.h"
#include "irq.h"
#include <string.h>

typedef volatile unsigned int vu32;

#define CD_BASE 0x88060000u

#define CD_DSP_ADDR  (*(vu32 *)(CD_BASE + 0x04))
#define CD_DSP_DATA  (*(vu32 *)(CD_BASE + 0x08))
#define CD_DSP_EXEC  (*(vu32 *)(CD_BASE + 0x0c))
#define CD_CTRL0     (*(vu32 *)(CD_BASE + 0x40))
#define CD_CTRL1     (*(vu32 *)(CD_BASE + 0x44))
#define CD_SEEK_MIN  (*(vu32 *)(CD_BASE + 0x48))
#define CD_SEEK_SEC  (*(vu32 *)(CD_BASE + 0x4c))
#define CD_SEEK_FRM  (*(vu32 *)(CD_BASE + 0x50))
#define CD_BUF_START (*(vu32 *)(CD_BASE + 0x60))
#define CD_BUF_END   (*(vu32 *)(CD_BASE + 0x64))
#define CD_BUF_PTR   (*(vu32 *)(CD_BASE + 0x68))
#define CD_SECSIZE   (*(vu32 *)(CD_BASE + 0x6c))

/* servo DSP registers */
#define DSP_SKIP_EXEC   0x013
#define DSP_SPEED       0x020
#define DSP_DISC_ID     0x030
#define DSP_VERSION     0x032
#define DSP_STATUS      0x079
#define DSP_DATA_HI     0x07c
#define DSP_DATA_LO     0x07d
#define DSP_FRAME_FOUND 0x307

#define CD_RAW_SECTOR   2352
#define CD_HEADER_BYTES 16      /* 12 sync + 4 header, then 2048 user bytes */
#define CD_PREGAP       150     /* LBA 0 sits at absolute MSF 00:02:00 */

/*
 * The ring, in the uncached window so the CPU never reads a stale copy of what
 * the servo just DMA'd in.
 *
 * It used to hold one frame, which made delivery a hard real-time contract:
 * copy each sector out before the next lands on top of it, or lose it silently.
 * That was survivable while the only interrupt was a pad poll, and stopped
 * being survivable the moment the mixer moved into an interrupt — a fill is
 * milliseconds and a sector is hundreds of microseconds.
 *
 * 1.15 MB out of ~14.5 MB of heap, and it now does two jobs: it lets an
 * interrupt handler run for milliseconds in the middle of a transfer, and it is
 * the bank the servo fills ahead of the caller while the CPU decodes.
 *
 * The three marks below are the whole prefetch policy:
 *
 *   HIGH   the servo's own interrupt parks it here, so the run is bounded by
 *          something that cannot be late. 448 frames.
 *   LOW    the reader restarts it here, so a restart costs one rotation rather
 *          than a starved decoder. 256 frames.
 *   MARGIN what is left above HIGH — 64 frames, 213 ms at 4x. This is how long
 *          interrupts may be masked before the servo could overwrite a sector
 *          nobody has counted, and it doubles as the threshold that *detects*
 *          that case: see servo_advance().
 */
#define CD_RING_FRAMES   512
#define CD_MARGIN_FRAMES 64
#define CD_HIGH_FRAMES   (CD_RING_FRAMES - CD_MARGIN_FRAMES)
#define CD_LOW_FRAMES    (CD_RING_FRAMES / 2)

#define CD_RING_BYTES   (CD_RING_FRAMES * CD_RAW_SECTOR)
#define CD_MARGIN_BYTES (CD_MARGIN_FRAMES * CD_RAW_SECTOR)
#define CD_HIGH_BYTES   (CD_HIGH_FRAMES * CD_RAW_SECTOR)
#define CD_LOW_BYTES    (CD_LOW_FRAMES * CD_RAW_SECTOR)

static unsigned char cd_ring[CD_RING_BYTES] __attribute__((aligned(4)));

/* Not static: the Lua harnesses read these by name out of a running machine,
 * and seeks are the number this whole file is shaped around. */
unsigned int cd_sector_count;
unsigned int cd_seek_count;
unsigned int cd_overrun_count;

/* Prefetch instrumentation. `cd_stall_count` is the one that says whether any
 * of this works: it counts sectors the reader had to wait for, so a load where
 * the bank was always ahead reports zero and a load where the servo never got
 * ahead reports one per sector. */
unsigned int cd_stall_count;
unsigned int cd_park_count;
unsigned int cd_resume_count;
unsigned int cd_servo_irqs;

/*
 * Sectors the mech actually pulled off the disc, counted from the write
 * pointer and not from cd_servo_irqs — which undercounts, because MAME's CPU
 * core ORs a pending bit (`m_pending_interrupt |= 1ULL << inputnum`) and two
 * sectors landing before the handler runs raise one interrupt between them.
 * The difference against cd_sector_count is the speculative read-ahead: free
 * in wall-clock, since the head would have been idle, but not free in the
 * mech's duty cycle.
 */
unsigned int cd_fetched_total;

/* The stream in flight. `stream_lba` is the disc address of the first sector
 * the servo wrote after arming, so the next sector to hand a caller is at
 * stream_lba + cd_consumed/CD_RAW_SECTOR and the next sector the *servo* will
 * write is at stream_lba + cd_produced/CD_RAW_SECTOR. Both counters are byte
 * counts since arming, which is what makes them directly comparable with the
 * servo's write pointer.
 *
 * `cd_produced` and `last_ptr` are advanced from the servo interrupt,
 * `cd_consumed` only by the reader, so no single word has two writers — the one
 * place that breaks the rule is stream_arm(), which resets all of them and is
 * therefore only ever called with interrupts masked.
 *
 * Not static, unlike the rest of the stream state: the pair is the bank level,
 * and a harness that cannot see it cannot tell a prefetch that is working from
 * one that never gets more than a sector ahead. */
static int                   stream_on;
static volatile int          stream_parked;
static unsigned int          stream_lba;
volatile unsigned int        cd_produced;
volatile unsigned int        cd_consumed;
static volatile unsigned int last_ptr;

/* Set when the write pointer has moved further between two looks than the
 * margin allows, which means interrupts were masked long enough for the servo
 * to run unwatched. What is in the ring can no longer be trusted to be what we
 * think it is, so the reader re-seeks rather than copying it. */
static volatile int servo_lost;

/*
 * Set while the servo is streaming.
 *
 * This used to be an order: the ring was one frame, so any handler that held
 * the CPU for longer than a sector destroyed data, and callers were expected to
 * stand aside. The ring is now CD_RING_FRAMES deep and an overrun re-seeks
 * instead of corrupting, so the flag is information rather than a rule. The pad
 * poll in src/platform_in.c and the mixer in src/audio.c both read it, and
 * neither has to obey it.
 */
volatile int cd_streaming;

static unsigned int uncached(const void *p)
{
    return ((unsigned int)p & 0x1fffffffu) | 0xa0000000u;
}

/*
 * The DSP port is three registers used as one transaction — address, data,
 * then exec — so it is a critical section, and it became one the moment the
 * servo got a handler: cd_servo_isr() acks through this same port, and an
 * interrupt landing between the address and the exec would retarget whatever
 * the interrupted code was in the middle of asking for. Masking here rather
 * than at each call site is what makes that impossible to get wrong later; the
 * port is never on a hot path.
 */
static void dsp_write(unsigned int addr, unsigned int data)
{
    unsigned int state = irq_disable();

    CD_DSP_ADDR = addr;
    CD_DSP_DATA = data;
    CD_DSP_EXEC = 0;            /* bit 0 clear = DSP write */
    irq_restore(state);
}

static unsigned int dsp_read(unsigned int addr)
{
    unsigned int state = irq_disable();
    unsigned int data;

    CD_DSP_ADDR = addr;
    CD_DSP_EXEC = 1;            /* bit 0 set = DSP read */
    data = CD_DSP_DATA;
    irq_restore(state);
    return data;
}

static unsigned int dec2bcd(unsigned int v)
{
    return ((v / 10) << 4) | (v % 10);
}

static void cd_servo_isr(void);

int cd_init(void)
{
    unsigned int ring = uncached(cd_ring);

    CD_CTRL1     = 0x04;                 /* bit 2: hold delivery while we set up */
    CD_CTRL0     = 0;                    /* data mode (bit 15 would select CDDA) */
    CD_SECSIZE   = CD_RAW_SECTOR;
    CD_BUF_START = ring;
    CD_BUF_END   = ring + CD_RING_BYTES - 1;   /* inclusive end */
    CD_BUF_PTR   = ring;

    /*
     * 1 << n, so 4x.
     *
     * The only direct claim about this mechanism anyone has produced is a forum
     * remark that the HyperScan shipped a 4x drive "when 40-50x units were
     * commonplace in 2006". Weak evidence, but it is evidence, and it beats the
     * two alternatives on offer: 8x, which nothing supports and which this file
     * asked for only because it measured better, and 1x-2x, which was inferred
     * from the console's notorious loading times.
     *
     * Worth following where that inference now leads. If the drive really is 4x
     * — 600 KB/s — then throughput never explained those loads, and what is left
     * is seek time. That agrees with what this port measured for itself in m7:
     * a naive read pattern issued about nine thousand seeks to load the game
     * where the read-ahead run issues seventy-eight, and at 100-200 ms apiece
     * that is the difference between twelve seconds and twenty minutes. The
     * legend is about the head, not the data rate.
     *
     * At 4x every movie fits: the heaviest sustained rate measured across the
     * twenty-three is DRAGON3 at 384 KB/s.
     */
    dsp_write(DSP_SPEED, 2);

    /* Call irq_init() first. Without the handler the servo still delivers and
     * the reader still polls, but nothing bounds a run the reader is not
     * watching, so the prefetch would be exactly the unsafe thing described
     * above cd_read(). */
    irq_set_handler(IRQ_CDSERVO, cd_servo_isr);

    dsp_write(DSP_DISC_ID, 0);
    return (int)((dsp_read(DSP_DATA_HI) << 8) | dsp_read(DSP_DATA_LO));
}

unsigned int cd_dsp_version(void)
{
    dsp_write(DSP_VERSION, 0);
    return (dsp_read(DSP_DATA_HI) << 8) | dsp_read(DSP_DATA_LO);
}

unsigned int cd_sectors_read(void)
{
    return cd_sector_count;
}

unsigned int cd_seeks(void)
{
    return cd_seek_count;
}

unsigned int cd_overruns(void)
{
    return cd_overrun_count;
}

unsigned int cd_stalls(void)
{
    return cd_stall_count;
}

unsigned int cd_banked(void)
{
    unsigned int state = irq_disable();
    unsigned int fill = cd_produced - cd_consumed;

    irq_restore(state);
    return fill / CD_RAW_SECTOR;
}

/* Point the seek registers at an absolute LBA. Does not arm. */
static void seek_to(unsigned int lba)
{
    unsigned int abs = lba + CD_PREGAP;

    CD_SEEK_MIN = dec2bcd(abs / (60 * 75));
    CD_SEEK_SEC = dec2bcd((abs / 75) % 60);
    CD_SEEK_FRM = dec2bcd(abs % 75);
}

/*
 * Bring `cd_produced` up to date, and park the servo if the bank is full.
 *
 * Two things are deliberately *not* done here. It does not count interrupts:
 * arrivals come from the movement of the write pointer, because a position can
 * be read late and still be right, while a count of edges cannot survive a
 * missed or a duplicated one. And it does not trust the frame-found flag, which
 * only says "at least one arrived since you last cleared it" — the moment the
 * CPU is late by more than a sector that flag makes it copy the same frame
 * twice while the disc moves on.
 *
 * What the interrupt buys is the *cadence*: called once per delivered sector,
 * this can never be looking at a pointer that has lapped, which is exactly the
 * precondition the arithmetic needs and the one an unwatched servo destroys.
 * When that precondition is violated anyway — interrupts masked for longer than
 * the margin — the same arithmetic notices, because a legitimate step is one
 * sector and this one would be enormous.
 *
 * Safe to call from either the interrupt or the reader, so long as the reader
 * masks interrupts around it: it is idempotent with respect to pointer
 * movement, so an extra call measures zero, but it is a read-modify-write and
 * must not be cut in half.
 */
static void servo_advance(void)
{
    unsigned int ptr = CD_BUF_PTR;
    unsigned int moved = ptr - last_ptr;

    if (ptr < last_ptr)
        moved += CD_RING_BYTES;          /* wrapped at CD_BUF_END */

    if (moved > CD_MARGIN_BYTES)
        servo_lost = 1;

    cd_produced += moved;
    cd_fetched_total += moved / CD_RAW_SECTOR;
    last_ptr = ptr;

    /* The high-water mark. This is the reason the prefetch is allowed to run
     * unattended at all: the bound is applied by the device's own interrupt,
     * once per sector, instead of by a reader that may not come back for
     * seconds. */
    if (stream_on && !stream_parked
        && cd_produced - cd_consumed >= CD_HIGH_BYTES) {
        CD_CTRL1 = 0x04;
        stream_parked = 1;
        cd_park_count++;
    }
}

/*
 * Vector 60, one per delivered sector.
 *
 * The ack is a write of 0 to DSP register 0x307, the frame-found flag — the
 * only per-sector state the servo exposes, and so the only thing that could
 * hold a level-triggered line up. MAME's CPU core takes interrupt lines as
 * edges (`execute_set_input` sets a pending bit and ignores CLEAR_LINE), so
 * under the emulator this write changes nothing and cannot be validated; it is
 * here because on real hardware not retiring the flag is the failure mode that
 * would show up as a livelock in the handler, and that is not a thing to
 * discover on hardware. The SoC's interrupt controller at 0x080a0000 is not
 * emulated either, so if a second ack is needed there, this driver does not
 * yet know about it.
 */
static void cd_servo_isr(void)
{
    dsp_write(DSP_FRAME_FOUND, 0);
    cd_servo_irqs++;

    if (stream_on)
        servo_advance();
}

/* servo_advance() from outside the handler. */
static void servo_poll(void)
{
    unsigned int state = irq_disable();

    servo_advance();
    irq_restore(state);
}

/* Start a fresh stream at `lba`: a real seek, and everything prefetched so far
 * is discarded. Writes every shared word, so callers must mask interrupts. */
static void stream_arm(unsigned int lba)
{
    CD_CTRL1 = 0x04;                     /* hold while repositioning */
    seek_to(lba);
    dsp_write(DSP_FRAME_FOUND, 0);

    CD_BUF_PTR = CD_BUF_START;
    last_ptr   = CD_BUF_START;
    cd_produced   = 0;
    cd_consumed   = 0;
    stream_lba = lba;
    stream_on  = 1;
    stream_parked = 0;
    servo_lost = 0;
    cd_seek_count++;

    CD_CTRL1 = 0;                        /* arm: latches the seek */
}

/*
 * Pick a parked stream back up where it stopped. The write pointer is left
 * alone so the ring simply carries on filling.
 *
 * Not counted as a seek, and the distinction is the point of counting at all:
 * the head is already on the sector it stopped at, so this costs a rotation,
 * not the 100-200 ms that makes cd_seek_count worth watching. The seek
 * registers have to be rewritten first regardless — any write to CD_CTRL1
 * re-derives the target from them, so leaving them alone would rewind the
 * stream to where it was originally armed.
 */
static void stream_resume(void)
{
    seek_to(stream_lba + cd_produced / CD_RAW_SECTOR);
    stream_parked = 0;
    cd_resume_count++;
    CD_CTRL1 = 0;
}

/*
 * Read sectors, out of the bank where possible.
 *
 * The servo is left running when this returns. That is the whole shape of the
 * driver, and it was not available until vector 60 existed: servo_advance()
 * infers arrivals from the movement of the write pointer, so it can only
 * measure absences shorter than one lap of the ring, and with the servo free
 * the absences are set by the game rather than by us — this port measured
 * four-second gaps between reads during the intro, against 1.7 s for a
 * 512-frame ring at 4x. The pointer wraps more than once, `cd_produced` silently
 * loses whole laps, the overrun test therefore *passes*, and the caller is
 * handed sectors from somewhere else on the disc while the counter insists
 * everything is fine. Garbled video, garbled audio, clean diagnostics.
 *
 * There is no sector-count register to bound the run with. The bound has to
 * come from something that runs whether or not the game is paying attention,
 * and the servo's own interrupt is the only such thing: it parks the mech at
 * the high-water mark, and the reader restarts it at the low-water mark.
 */
int cd_read(unsigned int lba, void *dst, unsigned int count)
{
    unsigned char *out = (unsigned char *)dst;
    unsigned int base = uncached(cd_ring);

    cd_streaming = 1;

    while (count != 0) {
        unsigned int guard = 0;
        unsigned int state;
        int stalled = 0;

        /*
         * Acquire one sound sector. The validity test has to sit between the
         * wait and the copy, not before the wait: what invalidates the ring is
         * the servo running unwatched, and the longest the reader is ever away
         * from it is exactly while it is waiting.
         */
        for (;;) {
            state = irq_disable();

            /* Only a request that continues where the last one stopped can be
             * served from what the servo has been quietly banking. Anything
             * else is a genuine seek. */
            if (!stream_on || lba != stream_lba + cd_consumed / CD_RAW_SECTOR)
                stream_arm(lba);
            else
                servo_advance();

            /*
             * Two ways the sector we want can already be gone: the servo
             * lapped the ring, or it ran while nobody was counting. Re-arm
             * rather than copying what is now some other part of the disc —
             * silent corruption surfaces later as a broken HQR header and then
             * as a wild pointer, which is the worst possible way to learn
             * about it.
             *
             * A margin of one sector on the lap test, because a servo exactly
             * one ring ahead is already writing into the sector we are about
             * to copy.
             */
            if (servo_lost
                || cd_produced - cd_consumed > CD_RING_BYTES - CD_RAW_SECTOR) {
                cd_overrun_count++;
                stream_arm(lba);
                irq_restore(state);
                continue;
            }

            if (cd_produced - cd_consumed >= CD_RAW_SECTOR) {
                irq_restore(state);
                break;
            }

            if (stream_parked)
                stream_resume();
            irq_restore(state);

            if (!stalled) {
                cd_stall_count++;
                stalled = 1;
            }

            /* ~2 s of headroom at 1x; a real mech also needs spin-up time.
             * Not reset by a re-arm or a resume: the budget is per sector, and
             * a mech that needs more than one re-arm to deliver one sector is
             * not going to be rescued by a longer wait. */
            if (++guard > 8000000u) {
                CD_CTRL1 = 0x04;
                stream_on = 0;
                stream_parked = 1;
                cd_streaming = 0;
                return -1;
            }

            /* The interrupt is doing this too. Polling as well is what keeps
             * the driver honest for a caller that reads with interrupts
             * masked: it degrades to the old watched-servo behaviour, which is
             * safe precisely because a reader in this loop is never away for a
             * lap. */
            servo_poll();
        }

        memcpy(out, (const void *)(base + (cd_consumed % CD_RING_BYTES)
                                   + CD_HEADER_BYTES),
               CD_SECTOR_USER);
        out      += CD_SECTOR_USER;
        cd_consumed += CD_RAW_SECTOR;
        cd_sector_count++;
        lba++;
        count--;

        /* Low-water mark: restart a parked mech while there is still half a
         * bank to read from, so the rotation it costs is spent behind work the
         * caller is doing anyway rather than in front of a starved decoder. */
        if (stream_parked && cd_produced - cd_consumed <= CD_LOW_BYTES) {
            state = irq_disable();
            if (stream_parked)
                stream_resume();
            irq_restore(state);
        }
    }

#ifdef HS_CD_NO_PREFETCH
    /*
     * Build with -DHS_CD_NO_PREFETCH to park the mech on the way out, which is
     * what this driver did before vector 60 existed. Everything else stays —
     * the deep ring, the pointer tracking, the resume-instead-of-re-seek — so
     * an A/B between the two builds measures the prefetch and nothing else.
     * That mattered: the ring and the drive speed both changed in the same
     * window as this feature, and without the switch the gain could not be
     * told apart from those.
     */
    CD_CTRL1 = 0x04;
    stream_parked = 1;
#endif

    cd_streaming = 0;
    return 0;
}
