/*
 * watcom_compat.h — force-included (gcc -include) in front of every ENGINE
 * translation unit, and never in front of ours.
 *
 * LBA1's source is 1994 Watcom C for DOS. Three classes of thing have to be
 * neutralised before GCC will look at it: calling-convention keywords that no
 * longer exist, DOS/Watcom library names, and a couple of places where the
 * engine uses an identifier that newlib has since claimed.
 *
 * Modelled on the DS port's version, minus its fopen/malloc wrappers: there,
 * paths had to be rewritten for nitroFS and allocations accounted against a
 * 4 MB budget. Here src/fs.c already matches on the basename case-folded, and
 * src/syscalls.c hands out 14 MB, so plain fopen and malloc are correct.
 */
#ifndef HS_WATCOM_COMPAT_H
#define HS_WATCOM_COMPAT_H

/* Watcom calling-convention and memory-model keywords. */
#define cdecl
#define __far
#define __near
#define __loadds
#define huge

#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif

#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* newlib's <string.h> declares BSD index(); GIF.C and PCX.C use `index` as a
 * plain variable. Include the header first so the declaration is intact, then
 * rename the engine's identifier for the rest of the unit. */
#define index lba1_index

/* DOS-era case-insensitive compares. */
extern int strcasecmp(const char *, const char *);
extern int strncasecmp(const char *, const char *, size_t);

#define stricmp(a, b)      strcasecmp((const char *)(a), (const char *)(b))
#define strcmpi(a, b)      strcasecmp((const char *)(a), (const char *)(b))
#define strnicmp(a, b, n)  strncasecmp((const char *)(a), (const char *)(b), (n))

/*
 * On-disk structure packing.
 *
 * GCC 4.2.1's score backend does not implement `#pragma pack`: it answers
 * "#pragma pack(push[, id], <n>) is not supported on this target" and lays the
 * struct out with natural alignment regardless — and the engine build passes
 * -w, so the warning went by unseen. The earlier ports leaned on that pragma
 * for every structure that mirrors a file layout, so all of them read short
 * here. T_HEADER, the HQR block header, is 10 bytes on disc and became 12,
 * which left every Load_HQR() two bytes past the start of its compressed
 * stream: the pictures still half-resolved into something recognisable, which
 * is exactly why it read as a palette or blitter problem.
 *
 * PORT_PACKED goes on those structures alongside the pragma the other ports
 * use.
 */
#define PORT_PACKED __attribute__((__packed__))

/*
 * Frame regulator, in 50 Hz ticks per MainLoop iteration (PERSO.C).
 *
 * This is a redraw-rate limiter and NOT a game-speed control, which was worth
 * measuring rather than assuming. Built at 1 and at 3 and profiled with
 * tools/m10_newgame.lua, the repaint rate moves exactly as the constant says —
 * 29100 px/s against 14094, so ~34 redraws/s against 16.7 — and the speed
 * things move at on screen does not change at all. LBA's motion is driven from
 * TimerRef, not from the frame count, so the loop rate only buys smoothness.
 *
 * 1 leaves the engine free to render as fast as it can, which is the DOS
 * behaviour and the right default. Raise it only to trade smoothness for
 * headroom: EXTRA_CFLAGS=-DPORT_TICKS_PER_FRAME=n.
 */
#ifndef PORT_TICKS_PER_FRAME
#define PORT_TICKS_PER_FRAME 1
#endif

#ifndef _MAX_PATH
#define _MAX_PATH  260
#define _MAX_DRIVE 3
#define _MAX_DIR   256
#define _MAX_FNAME 256
#define _MAX_EXT   256
#endif

#endif /* HS_WATCOM_COMPAT_H */
