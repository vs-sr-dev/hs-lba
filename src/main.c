/*
 * HS-LBA — milestone m8: the engine's own entry point.
 *
 * Everything below the game is now in place, so this is deliberately thin:
 * bring up the platform in dependency order, mount the disc, and hand control
 * to lba_main() — PERSO.C's original `main`, renamed by the earlier ports.
 *
 * From here on the engine is in charge. It calls InitAdelineSystem, which
 * probes for drivers (all of which report "not installed" from platform*.c),
 * sets up its 50 Hz timer through InitTimer(), allocates Log via
 * InitGraphSvga(), and starts opening HQR files through plain fopen.
 *
 * The TRACE block records how far the boot got, because the interesting
 * failures here are the ones where the engine wanders off rather than
 * returning: a fault lands in intmsg() (src/irq.c) and leaves its cause and PC
 * where a Lua script can read them out of a wedged machine.
 */

#include "irq.h"
#include "timer.h"
#include "video.h"
#include "input.h"
#include "fs.h"
#include "heap.h"
#include "trace.h"
#include "cd.h"
#include "card.h"
#include "save.h"

#include <stdio.h>

/*  [0] marker           [3] heap used (KB)
 *  [1] TimerRef         [4] heap free (KB)
 *  [2] archive files    [5] CD seeks                                       */

void lba_main(int argc, unsigned char *argv[]);

static unsigned char arg0[] = "HYPER.EXE";
static unsigned char *fake_argv[] = { arg0, 0 };

int main(void)
{
    MARK(1);

    /* Interrupts first: the timer and the vblank both want one, and the
     * engine's clock has to be ticking before it starts measuring anything. */
    irq_init();
    video_init();
    InitTimer();
    MARK(2);

    if (hs_input_init() < 0)
        MARK(0x81);             /* no pad; the engine can still boot */

    if (cd_init() < 97 || fs_mount() != 0) {
        MARK(0xF8);             /* nothing to load: stop here, visibly */
        for (;;) { }
    }
    TRACE[2] = (unsigned)fs_count();
    MARK(3);

    /* The card driver times its pulses with a counted loop, so it has to be
     * calibrated against the 50 Hz tick — hence after InitTimer(). save_init()
     * then reads the card once, which is what lets the main menu offer
     * "continue" from a card the player left on the reader. */
    card_init();
    save_init();
    MARK(4);

    lba_main(1, fake_argv);

    /* The DOS game never returned from here. */
    MARK(0xEE);
    TRACE[1] = (unsigned)TimerRef;
    TRACE[3] = heap_used() / 1024u;
    TRACE[4] = heap_free() / 1024u;
    TRACE[5] = cd_seeks();
    for (;;) { }
    return 0;
}
