/*
 * timer.c — the engine's 50 Hz time base, on SPG290 timer 0.
 *
 * LBA does not want a wall clock. TimerRef is incremented by an interrupt and
 * read as a counter, and RestoreTimer() in P_ANIM.C *rewinds it backwards*
 * after a menu, so the platform tick may only ever do TimerRef++ — deriving it
 * from elapsed time would silently undo that rewind. Same rule the DS port
 * settled on (platform/nds/nds_sys.c).
 *
 * It also has to be an interrupt rather than a flag polled by the game loop.
 * The overflow bit is a flag, not a counter: with a full LBA redraw costing
 * tens of milliseconds, a loop polling once per frame would miss whole ticks
 * and the clock would run slow by however long the frame took.
 *
 * Choosing the clock. MAME models the timer source as 27 MHz / (N+1) with N in
 * bits 0-7 of P_TIMER_CLK_SEL, and runs a callback every single tick — asking
 * for the undivided 27 MHz would mean 27 million callbacks per emulated second
 * and an unusable emulator. N = 215 gives exactly 125 kHz, and 125000 / 2500 is
 * exactly 50 Hz with no rounding error at all: the counter runs up from the
 * preload to 0xffff, so 0x10000 - 63036 = 2500 ticks per period. 8 us of
 * resolution is also a sane base for anything else that wants this timer later.
 */

#include "timer.h"
#include "irq.h"

typedef volatile unsigned int vu32;

#define P_TIMER_CLK_SEL        (*(vu32 *)0x882100e4u)
#define P_TIMER0_CLK_CONF      (*(vu32 *)0x8821006cu)
#define P_TIMER0_MODE_CTRL     (*(vu32 *)0x88160000u)
#define P_TIMER0_CCP_CTRL      (*(vu32 *)0x88160004u)
#define P_TIMER0_PRELOAD_DATA  (*(vu32 *)0x88160008u)

#define C_TIMER_CLK_EN         0x00000001u   /* P_TIMERx_CLK_CONF */
#define C_TIMER_RST_DIS        0x00000002u   /* also reloads the counter */
#define C_TIMER_INT_FLAG       0x04000000u   /* P_TIMERx_MODE_CTRL */
#define C_TIMER_INT_EN         0x08000000u
#define C_TIMER_CTRL_EN        0x80000000u
#define C_TIMER_MODE_TIMER     0x00000000u   /* P_TIMERx_CCP_CTRL bits 30-31 */

#define TIMER_DIV     215                    /* 27 MHz / (215+1) = 125 kHz */
#define TIMER_CLK_HZ  125000u
#define TICK_HZ       50u
#define TIMER_PRELOAD (0x10000u - (TIMER_CLK_HZ / TICK_HZ))

/* Writing the mode register acks the overflow flag, but COMBINE_DATA stores the
 * whole word first — so the ack has to carry the run bits or it stops the
 * timer instead of acknowledging it. */
#define MODE_RUN      (C_TIMER_CTRL_EN | C_TIMER_INT_EN)
#define MODE_ACK      (MODE_RUN | C_TIMER_INT_FLAG)

volatile ULONG TimerRef = 0;
volatile ULONG TimerSystem = 0;
UWORD NbFramePerSecond = 0;
UWORD CmptFrame = 0;
UWORD WaitNbTicks = 1;
UWORD Cmpt_18 = 0;

static volatile int TimerOn = 0;
static volatile unsigned int ticks = 0;

static void timer_isr(void)
{
    P_TIMER0_MODE_CTRL = MODE_ACK;

    ticks++;

    if (!TimerOn)
        return;

    TimerSystem++;
    TimerRef++;             /* relative ++ only: RestoreTimer() rewinds it */

    if (--WaitNbTicks == 0) {
        WaitNbTicks = TICK_HZ;
        NbFramePerSecond = CmptFrame;
        CmptFrame = 0;
    }
}

void InitTimer(void)
{
    TimerRef = 0;
    TimerSystem = 0;
    WaitNbTicks = 1;
    CmptFrame = 0;
    NbFramePerSecond = 0;
    ticks = 0;

    /* Source clock first: changing it while the timer runs re-arms it. */
    P_TIMER_CLK_SEL = TIMER_DIV;

    P_TIMER0_MODE_CTRL    = 0;                    /* stop and clear any flag */
    P_TIMER0_CCP_CTRL     = C_TIMER_MODE_TIMER;
    P_TIMER0_PRELOAD_DATA = TIMER_PRELOAD;

    irq_set_handler(IRQ_TIMER, timer_isr);

    TimerOn = 1;
    P_TIMER0_MODE_CTRL = MODE_RUN;
    P_TIMER0_CLK_CONF  = C_TIMER_CLK_EN | C_TIMER_RST_DIS;   /* run + load */
}

void ClearTimer(void)
{
    TimerOn = 0;
    P_TIMER0_CLK_CONF  = 0;
    P_TIMER0_MODE_CTRL = 0;
    irq_set_handler(IRQ_TIMER, 0);
}

/* The engine never calls this in the CD build; the DOS version used it to
 * retune IRQ0 when the MIDI driver took over timing. */
void SetTimer(WORD divisor)
{
    (void)divisor;
}

WORD GetTimer(void)
{
    return (WORD)TimerRef;
}

unsigned int timer_ticks(void) { return ticks; }
unsigned int timer_hz(void)    { return TICK_HZ; }
