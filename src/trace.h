#ifndef HS_TRACE_H
#define HS_TRACE_H

/*
 * A fixed block of RAM that milestones mirror their state into, so a MAME Lua
 * script under tools/ can assert on values instead of reading a screenshot.
 * It sits in the 64 KB the memory map reserves between the heap ceiling and the
 * framebuffers, in the uncached window so the emulator sees the stores
 * immediately — and, more to the point, somewhere malloc can never reach.
 *
 * Slots 0-13 belong to whichever milestone is building; 14 and 15 are reserved
 * for faults, which have to be readable when the milestone itself is wedged.
 */
#include "memmap.h"

#define TRACE ((volatile unsigned int *)TRACE_BASE)

#define MARK(n)  do { TRACE[0] = 0xAA000000u | (n); } while (0)

#define TRACE_FAULT     14      /* 0xFA000000 | exception cause */
#define TRACE_FAULT_PC  15

#endif
