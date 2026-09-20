#ifndef HS_TIMER_H
#define HS_TIMER_H

/*
 * The engine's time base (LIB386/PLATFORM/DOS/LIB_SYS/TIMER.C + TIMER_A.C).
 * Names and semantics are the engine's, not ours — see src/timer.c.
 */

typedef unsigned long  ULONG;
typedef unsigned short UWORD;
typedef signed short   WORD;

extern volatile ULONG TimerRef;      /* 50 Hz, monotonic *only by convention* */
extern volatile ULONG TimerSystem;   /* 50 Hz, never rewound                  */
extern UWORD NbFramePerSecond;       /* frames counted over the last second   */
extern UWORD CmptFrame;              /* frames since the last second boundary */
extern UWORD WaitNbTicks;
extern UWORD Cmpt_18;

void InitTimer(void);
void ClearTimer(void);
void SetTimer(WORD divisor);
WORD GetTimer(void);

/* Bump once per rendered frame so NbFramePerSecond means something. */
#define TimerCountFrame()  do { CmptFrame++; } while (0)

/* Diagnostics: ticks the ISR has taken, and the raw tick rate it was set up
 * for. */
unsigned int timer_ticks(void);
unsigned int timer_hz(void);

#endif
