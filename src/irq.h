#ifndef HS_IRQ_H
#define HS_IRQ_H

/*
 * Interrupt plumbing for the S+core 7.
 *
 * The SDK ships the vector table and the register save/restore stub
 * (sdk/.../IRQ/Sys_isr.s) but never turns interrupts on: nothing anywhere sets
 * CP0_EXCPVEC or PSR.IE, so on a quickloaded image the vector base is still
 * 0x9f000000 from reset — unmapped — and the CPU would never take an interrupt
 * anyway. irq_init() supplies both halves.
 *
 * Vector numbers are the CPU input lines the SoC devices are wired to:
 */
#define IRQ_I2C      39
#define IRQ_PPU      53     /* vblank */
#define IRQ_TIMER    56     /* all six timers share this line */
#define IRQ_CDSERVO  60

#define IRQ_VECTORS  64

typedef void (*irq_handler_t)(void);

/* Point the exception vector at our table and enable interrupts at the CPU.
 * Call once, before installing handlers. */
void irq_init(void);

/* Install (or, with 0, remove) the handler for one vector. Handlers run with
 * interrupts disabled — the CPU clears PSR.IE on entry — so keep them short. */
void irq_set_handler(unsigned int vec, irq_handler_t fn);

/* PSR.IE around a critical section. irq_restore() takes what irq_disable()
 * returned; nesting is safe. */
unsigned int irq_disable(void);
void         irq_restore(unsigned int state);

/* Diagnostics. `spurious` counts interrupts taken on a vector with no handler,
 * which is the signature of a device left asserting its line. */
unsigned int irq_count(unsigned int vec);
unsigned int irq_total(void);
unsigned int irq_spurious(void);

#endif
