#ifndef HS_MEMMAP_H
#define HS_MEMMAP_H

/*
 * Where everything lives in the 16 MB of SDRAM.
 *
 * This exists because the SDK's arrangement does not survive contact with a
 * program that actually allocates. libgloss.c carves the heap out of a
 * `static unsigned char _heap[HEAPSIZE]` — an 8 MB array in .bss — which put
 * the heap at 0xA009C948..0xA089DE88 and swallowed the framebuffers at
 * 0xA0500000 whole. Nothing had noticed because nothing had called malloc yet;
 * the engine wants a couple of megabytes for itself and this port intends to
 * park ~8.7 MB of HQR data in RAM on top of that, so it would have failed as
 * soon as it mattered and in the most confusing possible way — a heap write
 * silently repainting the screen.
 *
 *   0xA0000000  +--------------------------------+
 *               |  BIOS-owned; the loader lives   |  576 KB, never touched
 *   0xA00901FC  |  here and puts us above it      |
 *               +--------------------------------+
 *               |  exception vectors (Sys_isr.s)  |
 *   0xA0091000  +--------------------------------+
 *               |  .text .data .bss               |
 *      _end     +--------------------------------+
 *               |                                 |
 *               |  heap: sbrk grows up from _end  |  ~14 MB
 *               |                                 |
 *   0xA0EF0000  +--------------------------------+
 *               |  TRACE block (debug mirror)     |  64 KB reserved
 *   0xA0F00000  +--------------------------------+
 *               |  1 x 640x480x16bpp framebuffer  |  600 KB
 *   0xA0F96000  +--------------------------------+
 *               |  stack, growing down from       |  ~424 KB
 *   0xA0FFFFF0  |  _stack in hyperscan_Prog.ld    |
 *               +--------------------------------+
 *
 * The heap ceiling is a constant rather than a computed gap on purpose: a
 * malloc that fails is a diagnosable event, a malloc that quietly reaches the
 * framebuffers is not.
 */

#define RAM_BASE        0xa0000000u
#define RAM_TOP         0xa1000000u

#define HEAP_END        0xa0ef0000u     /* sbrk stops here */

#define TRACE_BASE      0xa0ef0000u     /* 64 KB reserved, out of the heap */

/*
 * Framebuffers. At 640x480 one buffer is 600 KB and three of them would run
 * past the top of RAM, so that mode gets a single buffer — which is all the
 * game ever uses: presentation is single-buffered on purpose (see
 * platform_gfx.c), and video_flip()/video_back() are only reached by the m6/m7
 * milestone tests. The stack still has ~424 KB below _stack.
 */
#define FB_BASE         0xa0f00000u
#ifdef HS_VIDEO_QVGA
#define FB_BYTES        (320u * 240u * 2u)
#define FB_COUNT        3u
#else
#define FB_BYTES        (640u * 480u * 2u)
#define FB_COUNT        1u
#endif

#define STACK_TOP       0xa0fffff0u     /* _stack in the linker script */

#endif
