/*
 * m19_card.c — the RFID card driver, on its own.
 *
 * Everything in src/card.c is a guess about timing until something answers.
 * The 96-byte format is already proven on the host (tests/save/test_save.c);
 * what cannot be proven there is whether a pulse this code calls "a 1 bit" is
 * read as a 1 bit, and that needs a card model at the other end.
 *
 * So this milestone does not boot the game. It calibrates, sends a REQA, reads
 * the card, writes a pattern, reads it back, then writes a second pattern that
 * differs in only a few places to check that the diff path really does skip the
 * bytes that already match — which is the difference between a save that costs
 * two seconds of contact and one that costs a quarter of a second.
 *
 * Build and run:
 *   make MAIN=src/m19_card.c
 *   hyprscan.exe hyprscan -rompath "<roms>;build" -memc build/card_blank.bin \
 *       -quickload build/HYPER.EXE -autoboot_script tools/m19_card.lua \
 *       -autoboot_delay 0 -nothrottle -video none -seconds_to_run 120
 *
 * Run it a second time WITHOUT -memc: every step must report "no card" and
 * nothing must hang. Absence is the case MAME models correctly and the one the
 * player will hit most often.
 *
 * TRACE[0] carries the verdict; the numbers go to the console ring buffer.
 */

#include "irq.h"
#include "timer.h"
#include "trace.h"
#include "card.h"

#include <stdio.h>
#include <string.h>

/*  [0] marker            [4] bytes programmed, first write
 *  [1] spins per us      [5] bytes programmed, second write
 *  [2] card present      [6] transactions
 *  [3] ms for one read   [7] driver errors
 *                        [8] whole-card reads that fell back to per-byte    */

static unsigned char before[CARD_USER_SIZE];
static unsigned char after[CARD_USER_SIZE];
static unsigned char want[CARD_USER_SIZE];

static unsigned int ms_since(unsigned int t0)
{
    return (timer_ticks() - t0) * (1000u / 50u);
}

/* A pattern with no long runs, so a byte written to the wrong address shows up
 * rather than landing on an identical neighbour. */
static void fill_pattern(unsigned char *p, unsigned int seed)
{
    unsigned int i;

    for (i = 0; i < CARD_USER_SIZE; i++)
        p[i] = (unsigned char)((i * 37u + seed * 101u + 5u) & 0xffu);
}

static int compare(const unsigned char *card_bytes, const unsigned char *wanted)
{
    unsigned int i, bad = 0;

    for (i = 0; i < CARD_USER_SIZE; i++) {
        if (card_bytes[i] == wanted[i])
            continue;

        if (bad < 8)
            printf("  byte %u: card %02x, wanted %02x\n",
                   i, card_bytes[i], wanted[i]);
        bad++;
    }

    if (bad)
        printf("  %u byte(s) wrong\n", bad);

    return bad == 0;
}

/* One whole save, timed and counted the way a real one will be. */
static int write_pattern(const unsigned char *pattern, unsigned int *programmed)
{
    unsigned int t0 = timer_ticks();
    unsigned int base = card_bytes_written;
    int steps, ok;

    steps = card_write_begin(pattern);
    if (steps < 0) {
        printf("  card_write_begin failed\n");
        return 0;
    }

    printf("  %d byte(s) differ\n", steps);

    while (card_write_step() > 0)
        { }

    ok = card_write_finish();
    *programmed = card_bytes_written - base;

    printf("  %s in %u ms, %u byte(s) programmed\n",
           ok ? "verified" : "FAILED", ms_since(t0), *programmed);

    return ok;
}

int main(void)
{
    unsigned int t0, first = 0, second = 0;
    int ok = 1;

    MARK(1);

    irq_init();
    InitTimer();

    card_init();
    TRACE[1] = card_calibration();
    printf("\nm19: delay loop calibrated at %u spins/us\n", card_calibration());

    /*
     * Presence first, and it is the only step allowed to report "no". Every
     * later one needs a card, so an absent card ends the run cleanly here
     * instead of leaving the rest to fail one at a time.
     */
    TRACE[2] = (unsigned)card_present();
    printf("m19: REQA -> %s\n", TRACE[2] ? "card present" : "no card");

    if (!TRACE[2]) {
        printf("m19: nothing on the reader; the absent-card path is what was "
               "tested and it did not hang\n");
        MARK(0x19);                     /* clean "no card" outcome */
        TRACE[6] = card_transactions;
        TRACE[7] = card_errors;
        for (;;) { }
    }

    t0 = timer_ticks();
    if (!card_read_user(before)) {
        printf("m19: whole-card read failed\n");
        ok = 0;
    } else {
        const unsigned char *uid = card_uid_bytes();

        TRACE[3] = ms_since(t0);
        printf("m19: read 96 user bytes in %u ms via %s; UID %02x %02x %02x "
               "%02x, user[0..7] %02x %02x %02x %02x %02x %02x %02x %02x\n",
               TRACE[3],
               card_rall_fallbacks ? "96 single READs (RALL refused)" : "RALL",
               uid[0], uid[1], uid[2], uid[3],
               before[0], before[1], before[2], before[3],
               before[4], before[5], before[6], before[7]);
    }

    if (ok) {
        printf("m19: writing pattern A\n");
        fill_pattern(want, 1);
        ok = write_pattern(want, &first);
        TRACE[4] = first;
    }

    if (ok) {
        ok = card_read_user(after) && compare(after, want);
        printf("m19: readback of A %s\n", ok ? "matches" : "DIFFERS");
    }

    /*
     * The same pattern with four bytes moved. A driver that ignored the diff
     * would program 95 bytes again and take four times as long; the count is
     * the assertion, not the timing, because the timing is the thing MAME gets
     * wrong.
     */
    if (ok) {
        printf("m19: writing pattern A with 4 bytes changed\n");
        want[5] ^= 0xff;
        want[40] ^= 0xff;
        want[41] ^= 0xff;
        want[94] ^= 0xff;

        ok = write_pattern(want, &second);
        TRACE[5] = second;

        /* Four payload bytes, plus the magic cleared at the start and written
         * back at the end. */
        if (ok && second != 6) {
            printf("m19: expected 6 byte-writes, got %u — the diff path is "
                   "not doing its job\n", second);
            ok = 0;
        }
    }

    if (ok) {
        ok = card_read_user(after) && compare(after, want);
        printf("m19: readback of the modified pattern %s\n",
               ok ? "matches" : "DIFFERS");
    }

    TRACE[6] = card_transactions;
    TRACE[7] = card_errors;
    TRACE[8] = card_rall_fallbacks;

    printf("m19: %s - %u transactions, %u bytes programmed, %u errors, "
           "%u RALL fallback(s)\n",
           ok ? "PASS" : "FAIL",
           card_transactions, card_bytes_written, card_errors,
           card_rall_fallbacks);

    MARK(ok ? 0x00 : 0xF9);
    for (;;) { }
    return 0;
}
