/*
 * card.c — the RFID save card, bit-banged on one GPIO pin.
 *
 * The console has no writable storage except this card: 120 bytes of a Type 1
 * (Topaz/Jewel) tag, of which bytes 8..0x67 are user data. The NOR flash is not
 * an alternative — it holds the firmware the machine boots from.
 *
 * THE WIRING. MAME's driver hangs the card off two registers that the SDK's own
 * header already names: the transmit line is bit 1 of P_CSI_GPIO_SETUP
 * (0x88200024), and the tag's answer arrives in P_CSI_GPIO_INPUT (0x88200068).
 * The same setup register carries the eight front-panel LEDs on bits 5..12, which
 * is where the 0x1FFE0000 output-enable mask below comes from: it is exactly what
 * the SDK's HS_LEDS() macro writes, i.e. the one pin configuration this board is
 * known to be happy with.
 *
 * (MAME also maps a one-word "card present" register at 0x08200070. That address
 * is P_IOB_GPIO_INPUT on real silicon and the MAME source labels it a homebrew
 * extension. Reading it would make card detection work in the emulator and
 * nowhere else, so card_present() below sends a real REQA instead.)
 *
 * THE PROTOCOL, as MAME models it. Only the HIGH time of each pulse carries
 * meaning, measured in cycles of the 13.56 MHz carrier:
 *
 *      < 500 cycles  (< 36.9 us)      a 0 bit
 *    500..999        (36.9..73.7 us)  a 1 bit
 *   1000..4999       (73.7..368.7 us) a read strobe: advance the answer by one
 *                                     half-bit
 *     >= 5000        (>= 368.7 us)    ignored
 *
 * The low time between pulses is unconstrained, which is the one piece of luck
 * in this: slow code between bits is free, and only the pulse itself has to be
 * timed. So interrupts are disabled around each individual pulse rather than
 * around the transaction. Around the transaction would mean a second and a half
 * with the 50 Hz tick and the CD servo switched off; per pulse costs at most
 * 165 us of latency and leaves a gap after every single bit.
 *
 * Answers are Manchester coded: the tag presents each bit twice, the second time
 * inverted, so one logical bit is two strobes. Every answer opens with fifteen 0
 * bits and a 1, which is what rx_sync() hunts for, and each byte that follows is
 * eight bits least-significant-first plus an odd parity bit.
 *
 * WHERE THE TIME GOES. A strobe is the expensive primitive, and reading is what
 * a strobe is for, so reading costs far more than writing:
 *
 *   REQA        7 bits out, a handful in                        ~1 ms
 *   RALL        124 bytes in, 2264 strobes                    ~370 ms
 *   WRITE-E     71 bits out, no answer read back, plus the
 *               tag's own programming time                     ~8 ms/byte
 *
 * Hence the shape of a save: read the whole card once, program only the bytes
 * that actually changed, and verify with a second whole-card read instead of
 * reading back each byte's echo. That is both cheaper and a stronger check —
 * 96 echoes cost 1.6 s and prove only that the tag repeated what it was told,
 * whereas one RALL costs 370 ms and proves what the tag now holds.
 *
 * MAME MODELS ABSENCE, NOT TIME. is_loaded() is checked on every transition, so
 * an unmounted card genuinely does not answer and "no card" is reproducible in
 * the emulator. Its writes, however, complete instantly, while a real tag wants
 * milliseconds of EEPROM programming per byte. T_PROGRAM_US below is that time,
 * and it is spent whether or not the emulator needs it: leaving it out would
 * make the emulated save four times faster than the real one and hide the fact
 * that the player has to hold the card still for over a second.
 */

#include "card.h"
#include "irq.h"
#include "timer.h"

#include <string.h>

typedef volatile unsigned int vu32;

#define P_CSI_GPIO_SETUP  (*(vu32 *)0x88200024u)
#define P_CSI_GPIO_INPUT  (*(vu32 *)0x88200068u)

/* Output enable for pins 1..12, the mask HS_LEDS() uses. The data half is the
 * low 16 bits; the card is pin 1, the LEDs are 5..12 and stay low. */
#define GPIO_OE           0x1FFE0000u
#define CARD_PIN          0x00000002u

/*
 * Pulse widths. Each sits near the geometric centre of its window rather than
 * the arithmetic one, because the error being guarded against is a ratio: the
 * delay loop is calibrated at runtime and what it can be is proportionally
 * fast or slow, not fast or slow by so many microseconds.
 *
 *   bit 0    window 0..36.9      -> 18 us, 2.0x of margin above
 *   bit 1    window 36.9..73.7   -> 52 us, 1.4x either way
 *   strobe   window 73.7..368.7  -> 165 us, 2.2x either way
 */
#define T_BIT0_US      18u
#define T_BIT1_US      52u
#define T_STROBE_US    165u
#define T_GAP_US       6u       /* low time; only has to be long enough to be
                                 * seen as a transition */

/*
 * How long a Type 1 tag spends programming one byte before it will listen
 * again. The datasheet figure for a Topaz WRITE-E cycle is a little over 5 ms.
 */
#define T_PROGRAM_US   5000u

/* Commands. The first byte of a frame is sent as SEVEN bits. */
#define CMD_REQA       0x26u
#define CMD_RID        0x78u
#define CMD_READ       0x01u
#define CMD_RALL       0x00u
#define CMD_WRITE_E    0x53u

unsigned int card_transactions;
unsigned int card_bytes_written;
unsigned int card_errors;
unsigned int card_rall_fallbacks;

static unsigned int gpio_shadow = GPIO_OE;   /* the register does not read back
                                              * on hardware the way it does in
                                              * MAME, so keep our own copy */
static unsigned char card_uid[4];

/* ---- the delay loop ----------------------------------------------------- */

/*
 * There is no readable free-running counter to spin on: timer 0 is the 50 Hz
 * tick, and MAME runs a callback for every single tick of a timer's source
 * clock, so a second timer fast enough to measure microseconds would make the
 * emulator unusable. That leaves a counted loop — and a counted loop has to be
 * calibrated, because 108 MHz with a cache is not a number you can divide by.
 *
 * So measure it against the one clock that is already correct on both the
 * console and the emulator: the 50 Hz interrupt. Four ticks is 80 ms, long
 * enough that the timer_ticks() call in the loop condition is noise.
 */
static volatile unsigned int spin_sink;
static unsigned int spins_per_us = 8;   /* replaced by card_init() */

static void spin(unsigned int n)
{
    while (n--)
        spin_sink = n;
}

static void udelay(unsigned int us)
{
    spin(us * spins_per_us);
}

void card_init(void)
{
    unsigned int t0, iters = 0;

    /* Park the line low and take the pin. Idle low matters: MAME's card only
     * updates its idea of the line level while a card is loaded, so a card
     * mounted mid-session has to find the line where it last left it. */
    gpio_shadow = GPIO_OE;
    P_CSI_GPIO_SETUP = gpio_shadow;

    t0 = timer_ticks();
    while (timer_ticks() == t0)
        { }                             /* start on a tick boundary */

    t0 = timer_ticks();
    while (timer_ticks() - t0 < 4) {
        spin(256);
        iters += 256;
    }

    spins_per_us = iters / 80000u;      /* 4 ticks at 50 Hz = 80000 us */
    if (spins_per_us == 0)
        spins_per_us = 1;
}

unsigned int card_calibration(void) { return spins_per_us; }

/* ---- bits --------------------------------------------------------------- */

/*
 * One pulse. The high time is the payload, so it is the only part that must not
 * be stretched by an interrupt; the low gap afterwards is deliberately outside
 * the critical section, which is where the timer and the CD servo get their
 * chance to run.
 */
static void pulse(unsigned int high_us)
{
    unsigned int state = irq_disable();

    P_CSI_GPIO_SETUP = gpio_shadow | CARD_PIN;
    udelay(high_us);
    P_CSI_GPIO_SETUP = gpio_shadow;

    irq_restore(state);

    udelay(T_GAP_US);
}

static void tx_bit(int one)
{
    pulse(one ? T_BIT1_US : T_BIT0_US);
}

/* The first byte of every frame is seven bits wide; the rest are eight. */
static void tx_first(unsigned char b)
{
    int i;
    for (i = 0; i < 7; i++)
        tx_bit((b >> i) & 1);
}

static void tx_byte(unsigned char b)
{
    int i;
    for (i = 0; i < 8; i++)
        tx_bit((b >> i) & 1);
}

/* Sample the half-bit the tag is presenting, then step past both halves. */
static int rx_bit(void)
{
    int v = (int)(P_CSI_GPIO_INPUT & 1u);

    pulse(T_STROBE_US);
    pulse(T_STROBE_US);
    return v;
}

/*
 * Hunt for the end of the preamble. An answer begins with fifteen 0 bits and a
 * 1; no card at all reads as a permanent 0, so the bounded search below is also
 * what turns silence into a clean failure instead of a hang.
 */
static int rx_sync(void)
{
    int i;

    for (i = 0; i < 40; i++)
        if (rx_bit())
            return 1;

    return 0;
}

static int rx_byte(unsigned char *out)
{
    unsigned int b = 0;
    int i, parity = 1, v;

    for (i = 0; i < 8; i++) {
        v = rx_bit();
        b |= (unsigned int)v << i;
        parity ^= v;
    }

    v = rx_bit();
    *out = (unsigned char)b;

    return v == parity;
}

/* ---- frames ------------------------------------------------------------- */

/*
 * The CRC the tag expects: ISO 14443-B, seeded 0xffff and inverted at the end.
 * MAME does not check it. Real tags do, so it is computed properly here — this
 * is the same class of mistake as reading MAME's card-detect register, a thing
 * that would work perfectly right up until it ran on the console.
 */
static unsigned int crc16(const unsigned char *data, unsigned int len)
{
    unsigned int crc = 0xffffu;

    while (len--) {
        unsigned int b = *data++;

        b ^= crc & 0xffu;
        b ^= (b << 4) & 0xffu;

        crc = (crc >> 8) ^ ((b << 8) & 0xffffu) ^ ((b << 3) & 0xffffu) ^ (b >> 4);
    }

    return (~crc) & 0xffffu;
}

/*
 * Every command but REQA is nine bytes: opcode, address, data, the four-byte
 * UID and the CRC. The UID has to be right for a real tag to answer, which is
 * why card_write_begin() runs an RID first and everything else refuses to
 * transmit until it has one.
 */
static void tx_frame(unsigned char cmd, unsigned char addr, unsigned char data)
{
    unsigned char f[7];
    unsigned int crc;

    f[0] = cmd;
    f[1] = addr;
    f[2] = data;
    memcpy(f + 3, card_uid, 4);

    crc = crc16(f, 7);

    card_transactions++;

    tx_first(f[0]);
    tx_byte(f[1]);
    tx_byte(f[2]);
    tx_byte(f[3]);
    tx_byte(f[4]);
    tx_byte(f[5]);
    tx_byte(f[6]);
    tx_byte((unsigned char)(crc & 0xffu));
    tx_byte((unsigned char)(crc >> 8));
}

/* ---- transactions ------------------------------------------------------- */

int card_present(void)
{
    card_transactions++;

    tx_first(CMD_REQA);

    /* The ATQA itself says nothing useful — MAME answers 0x0000 and a real tag
     * answers something else. What is being tested is whether anything at all
     * drove the line, and the preamble's single 1 bit is enough for that. */
    return rx_sync();
}

/*
 * RID, to learn the UID. Without it the nine-byte frames above are addressed to
 * nobody. MAME ignores the UID field, so this step is invisible in the emulator
 * and load-bearing on the console.
 */
static int card_identify(void)
{
    unsigned char b[6];
    int i;

    if (!card_present())
        return 0;

    tx_frame(CMD_RID, 0, 0);

    if (!rx_sync())
        return 0;

    for (i = 0; i < 6; i++) {
        if (!rx_byte(&b[i])) {
            card_errors++;
            return 0;
        }
    }

    memcpy(card_uid, b + 2, 4);         /* b[0], b[1] are HR0 and HR1 */
    return 1;
}

const unsigned char *card_uid_bytes(void) { return card_uid; }

/*
 * RALL. The whole tag in one answer, which is the only reason a load is a
 * third of a second rather than two.
 *
 * Note the length: 122 bytes of answer is 1114 bits, and each bit is two
 * strobes. MAME cannot deliver that — its m_resp_idx is a uint8_t, so after
 * 256 half-bits the answer silently starts again from the beginning, and the
 * parity check below is what notices. That is an emulator defect and not
 * something a real Type 1 tag does; a patch for it is in docs/, and the
 * fallback in card_read_user() is there so that neither this driver nor the
 * game ever depends on that patch being applied.
 */
static int card_rall(unsigned char *buf120)
{
    unsigned char hr[2];
    unsigned int i;

    tx_frame(CMD_RALL, 0, 0);

    if (!rx_sync())
        return 0;

    if (!rx_byte(&hr[0]) || !rx_byte(&hr[1]))
        return 0;

    for (i = 0; i < CARD_BYTES; i++)
        if (!rx_byte(&buf120[i]))
            return 0;

    return 1;
}

/* One byte, addressed. Five times slower per byte than RALL is per card. */
static int card_read_one(unsigned int addr, unsigned char *out)
{
    unsigned char a, d;

    tx_frame(CMD_READ, (unsigned char)addr, 0);

    if (!rx_sync())
        return 0;

    if (!rx_byte(&a) || !rx_byte(&d))
        return 0;

    if (a != (unsigned char)addr)
        return 0;

    *out = d;
    return 1;
}

int card_read_user(unsigned char *buf)
{
    unsigned char all[CARD_BYTES];
    unsigned int i;

    /* Identify every time rather than caching: the UID costs a millisecond
     * against this call's several hundred, and a stale one would address a
     * card the player has already swapped. */
    if (!card_identify())
        return 0;

    if (card_rall(all)) {
        memcpy(buf, all + CARD_USER_BASE, CARD_USER_SIZE);
        return 1;
    }

    /*
     * The tag would not stream the whole card. Rather than fail a load for it,
     * fetch the user area a byte at a time — but count it, because the two
     * paths differ by nearly two seconds and a load that quietly became five
     * times slower is exactly the kind of regression that goes unnoticed.
     */
    card_rall_fallbacks++;

    for (i = 0; i < CARD_USER_SIZE; i++) {
        if (!card_read_one(CARD_USER_BASE + i, &buf[i])) {
            card_errors++;
            return 0;
        }
    }

    return 1;
}

/*
 * One byte, programmed and not read back. The answer is left undrained on
 * purpose: the next frame's response resets the tag's output index anyway, and
 * reading the echo would double what a save costs to learn less than the
 * whole-card verify in card_write_finish() already establishes.
 */
static void card_program(unsigned int addr, unsigned char value)
{
    tx_frame(CMD_WRITE_E, (unsigned char)addr, value);
    udelay(T_PROGRAM_US);
    card_bytes_written++;
}

/* ---- the write sequence ------------------------------------------------- */

/*
 * Order matters here and it is the one thing about this file that cannot be
 * tested in an emulator, because it is about what the card holds when the
 * player lifts it early.
 *
 *   1. read the card
 *   2. clear the magic          <- from here the card reads as "no save"
 *   3. program the payload
 *   4. read it all back and compare
 *   5. write the magic          <- and only now does it read as a save again
 *
 * Programming the payload first and the magic last is not enough on its own:
 * the OLD magic would still be sitting there through step 3, vouching for a
 * half-overwritten payload. Clearing it first costs one extra byte-write and
 * makes a torn save indistinguishable from an empty card, which is the correct
 * thing for it to look like.
 */
static const unsigned char *want_image;
static unsigned char        card_now[CARD_USER_SIZE];
static unsigned int         write_cursor;

int card_write_begin(const unsigned char *want)
{
    unsigned int i, todo = 0;

    want_image = want;
    write_cursor = 1;                   /* byte 0 is the magic, written last */

    if (!card_read_user(card_now)) {
        want_image = 0;
        return -1;
    }

    for (i = 1; i < CARD_USER_SIZE; i++)
        if (card_now[i] != want[i])
            todo++;

    if (card_now[0] != 0) {
        card_program(CARD_USER_BASE, 0);
        card_now[0] = 0;
    }

    return (int)todo;
}

int card_write_step(void)
{
    unsigned int i;

    if (!want_image)
        return -1;

    for (i = write_cursor; i < CARD_USER_SIZE; i++) {
        if (card_now[i] == want_image[i])
            continue;

        card_program(CARD_USER_BASE + i, want_image[i]);
        card_now[i] = want_image[i];
        write_cursor = i + 1;
        break;
    }

    for (i = write_cursor; i < CARD_USER_SIZE; i++)
        if (card_now[i] != want_image[i])
            return 1;                   /* more to do */

    write_cursor = CARD_USER_SIZE;
    return 0;
}

int card_write_finish(void)
{
    const unsigned char *want = want_image;
    unsigned char data;
    unsigned int i;

    want_image = 0;

    if (!want)
        return 0;

    if (!card_read_user(card_now))
        return 0;

    for (i = 1; i < CARD_USER_SIZE; i++) {
        if (card_now[i] != want[i]) {
            card_errors++;
            return 0;
        }
    }

    card_program(CARD_USER_BASE, want[0]);

    /* One READ to confirm the magic landed. Cheap, and it is the byte the
     * whole format's validity hangs on. */
    if (!card_read_one(CARD_USER_BASE, &data) || data != want[0]) {
        card_errors++;
        return 0;
    }

    return 1;
}
