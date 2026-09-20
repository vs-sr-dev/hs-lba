/*
 * irq.c — interrupt dispatch.
 *
 * Sys_isr.s owns the hard part: a vector table linked at 0xA00901FC (the load
 * address of HYPER.EXE itself, so the vectors are the first bytes of the
 * image), and a save_reg stub that spills r1-r31 plus CR and EPC, passes the
 * exception cause register in r4, and returns through `rte`. The vectors for
 * IRQ24..63 all funnel into it.
 *
 * What was missing is everything around it:
 *
 *   - CP0_EXCPVEC still holds its reset value, 0x9f000000, which the SoC does
 *     not map. The CPU forms the target as (EXCPVEC & 0xffff0000) + 0x200 +
 *     (vector << 2), so the register has to name our table's page, 0xa0090000:
 *     0xa0090000 + 0x200 + 56*4 lands on int56_vec. The low bit of EXCPVEC
 *     picks 4- or 16-byte vector spacing and must stay clear, matching the
 *     `.align 2` in the table.
 *   - PSR.IE is clear out of reset and nothing sets it, so pending interrupts
 *     just accumulate. That is why m5 could leave the I2C interrupt enabled at
 *     the device and still be sure it was doing polled I/O.
 *
 * This file also replaces the SDK's Sys_IRQ.c/User_IRQ.c pair: a 64-way switch
 * calling forty empty functions, with an `intmsg()` that is an infinite loop.
 * A table of function pointers does the same job and lets a driver claim its
 * own vector.
 */

#include "irq.h"
#include "trace.h"

/* Page containing .exception_vec — see the `.exception 0xA00901fc` output
 * section in sdk/boot/hyperscan_Prog.ld. */
#define EXCPVEC_BASE  0xa0090000u

/* mtcr takes five slots to land. The assembler diagnoses a short gap as a
 * "data dependency ... bubble" warning, which is worth keeping quiet: a stale
 * PSR read straight after enabling interrupts would be a genuinely nasty bug. */
#define CR_SETTLE  "nop\n\tnop\n\tnop\n\tnop\n\tnop"

#define PSR_IE        0x00000001u

static irq_handler_t handlers[IRQ_VECTORS];
static unsigned int  counts[IRQ_VECTORS];
static unsigned int  total;
static unsigned int  spurious;

/* Called from save_reg in Sys_isr.s with CP0_CAUSE in r4. Bits 18-23 carry the
 * vector the CPU dispatched on. */
void irq_dispatch(unsigned int cp0_cause)
{
    unsigned int vec = (cp0_cause >> 18) & 0x3f;
    irq_handler_t fn = handlers[vec];

    counts[vec]++;
    total++;

    if (fn)
        fn();
    else
        spurious++;
}

void irq_init(void)
{
    unsigned int i, psr;

    for (i = 0; i < IRQ_VECTORS; i++) {
        handlers[i] = 0;
        counts[i] = 0;
    }
    total = 0;
    spurious = 0;

    __asm__ __volatile__ ("mtcr %0, cr3\n\t" CR_SETTLE
                          : : "r" (EXCPVEC_BASE));

    __asm__ __volatile__ ("mfcr %0, cr0" : "=r" (psr));
    psr |= PSR_IE;
    __asm__ __volatile__ ("mtcr %0, cr0\n\t" CR_SETTLE
                          : : "r" (psr));
}

void irq_set_handler(unsigned int vec, irq_handler_t fn)
{
    if (vec < IRQ_VECTORS) {
        unsigned int s = irq_disable();
        handlers[vec] = fn;
        irq_restore(s);
    }
}

unsigned int irq_disable(void)
{
    unsigned int psr;

    __asm__ __volatile__ ("mfcr %0, cr0" : "=r" (psr));
    __asm__ __volatile__ ("mtcr %0, cr0\n\t" CR_SETTLE
                          : : "r" (psr & ~PSR_IE));
    return psr;
}

void irq_restore(unsigned int state)
{
    unsigned int psr;

    __asm__ __volatile__ ("mfcr %0, cr0" : "=r" (psr));
    psr = (psr & ~PSR_IE) | (state & PSR_IE);
    __asm__ __volatile__ ("mtcr %0, cr0\n\t" CR_SETTLE
                          : : "r" (psr));
}

unsigned int irq_count(unsigned int vec)
{
    return (vec < IRQ_VECTORS) ? counts[vec] : 0;
}

unsigned int irq_total(void)    { return total; }
unsigned int irq_spurious(void) { return spurious; }

/*
 * The dead-end vectors — debug, general exception, and IRQ1..23 — all `jl
 * intmsg` in Sys_isr.s, where the SDK's version was a bare `while(1);`. Those
 * are real faults (bus error, unaligned access, illegal instruction), and a
 * silent freeze is the worst possible way to report one, so record the cause
 * and the return address where a Lua script can still read them out of a wedged
 * machine before halting.
 */
void intmsg(void)
{
    unsigned int ecr, epc;

    __asm__ __volatile__ ("mfcr %0, cr2" : "=r" (ecr));
    __asm__ __volatile__ ("mfcr %0, cr5" : "=r" (epc));

    TRACE[TRACE_FAULT]    = 0xFA000000u | (ecr & 0x00ffffffu);
    TRACE[TRACE_FAULT_PC] = epc;

    for (;;) { }
}
