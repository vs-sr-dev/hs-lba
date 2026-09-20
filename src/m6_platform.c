/*
 * HS-LBA — milestone m6: interrupts, the engine's clock, and a real frame loop.
 *
 * Up to m5 everything was a straight line: do a thing, spin, look at it. This
 * is the first build that runs the shape the engine actually runs in — read the
 * pad, advance on a 50 Hz tick that an interrupt owns, draw, flip in vblank —
 * and it is the first build where the CPU takes interrupts at all.
 *
 * The three pieces that had to exist first:
 *   src/irq.c    CP0_EXCPVEC + PSR.IE, and a dispatch table behind Sys_isr.s
 *   src/timer.c  TimerRef at exactly 50 Hz, incremented only by its ISR
 *   src/video.c  double buffer, flipped on the PPU's vblank interrupt
 *
 * On screen: a box driven by the stick, moved once per engine tick rather than
 * once per frame, so its speed is set by TimerRef and not by how fast the
 * machine happens to be drawing. That is exactly the property the engine needs
 * from the platform, and it is visible if you watch the box while the frame
 * rate changes.
 */

#include "TV/TV.h"
#include "irq.h"
#include "timer.h"
#include "video.h"
#include "input.h"

#include <stdio.h>
#include <string.h>

/*  [0] milestone marker      [5] vblanks
 *  [1] frames drawn          [6] IRQ total
 *  [2] TimerRef              [7] IRQ spurious
 *  [3] timer ticks           [8] pad buttons
 *  [4] NbFramePerSecond      [9] I2C timeouts                              */
#include "trace.h"

typedef unsigned int u32;

extern const unsigned char font[];

#define GREEN 0x07E0
#define RED   0xF800
#define WHITE 0xFFFF
#define GREY  0x8410
#define CYAN  0x07FF
#define BG    0x0000

static void hs_puts(unsigned short *fb, int cx, int cy, const char *s,
                    unsigned short fg, unsigned short bg)
{
    while (*s) {
        const unsigned char *g = &font[((unsigned char)*s) * 16];
        int yy;
        for (yy = 0; yy < 16; yy++) {
            unsigned short *d = fb + (cy * 16 + yy) * VIDEO_W + cx * 8;
            unsigned char row = g[yy];
            int xx;
            for (xx = 0; xx < 8; xx++)
                d[xx] = (row & (0x80 >> xx)) ? fg : bg;
        }
        cx++;
        s++;
    }
}

static void fill(unsigned short *fb, int x0, int y0, int w, int h,
                 unsigned short c)
{
    int y, x;

    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > VIDEO_W) w = VIDEO_W - x0;
    if (y0 + h > VIDEO_H) h = VIDEO_H - y0;

    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            fb[(y0 + y) * VIDEO_W + x0 + x] = c;
}

#define BOX 12

int main(void)
{
    const hs_pad *pad;
    ULONG last_tick;
    u32 frames = 0;
    int bx = (VIDEO_W - BOX) / 2;
    int by = (VIDEO_H - BOX) / 2;

    MARK(1);

    /* Interrupts before any driver that wants one. */
    irq_init();
    video_init();
    MARK(2);

    InitTimer();
    MARK(3);

    if (hs_input_init() < 0)
        MARK(0xF5);
    pad = hs_input_pad();

    last_tick = TimerRef;
    MARK(4);

    for (;;) {
        char line[48];
        ULONG now;

        hs_input_poll();

        /* Simulation advances per engine tick, never per frame. Catching up in
         * a loop is what keeps a slow frame from slowing the game down. */
        now = TimerRef;
        while (last_tick != now) {
            if (pad->dpad & HS_DPAD_LEFT)  bx -= 2;
            if (pad->dpad & HS_DPAD_RIGHT) bx += 2;
            if (pad->dpad & HS_DPAD_UP)    by -= 2;
            if (pad->dpad & HS_DPAD_DOWN)  by += 2;
            last_tick++;
        }

        if (bx < 0) bx = 0;
        if (by < 32) by = 32;
        if (bx > VIDEO_W - BOX) bx = VIDEO_W - BOX;
        if (by > VIDEO_H - BOX) by = VIDEO_H - BOX;

        video_clear(BG);
        {
            unsigned short *fb = video_back();

            hs_puts(fb, 1, 0, "HS-LBA  m6  irq + 50Hz tick", WHITE, BG);

            sprintf(line, "TimerRef %lu  ticks %u", TimerRef, timer_ticks());
            hs_puts(fb, 1, 2, line, GREY, BG);

            sprintf(line, "vblank %u  fps %u", video_vblanks(),
                    (unsigned)NbFramePerSecond);
            hs_puts(fb, 1, 3, line, GREY, BG);

            sprintf(line, "irq %u  spur %u  to %u", irq_total(),
                    irq_spurious(), hs_input_timeouts());
            hs_puts(fb, 1, 4, line,
                    irq_spurious() ? RED : GREY, BG);

            hs_puts(fb, 1, 13, "stick moves the box", CYAN, BG);

            fill(fb, bx, by, BOX, BOX, pad->buttons ? GREEN : WHITE);
        }

        video_flip();
        frames++;
        TimerCountFrame();

        TRACE[1] = frames;
        TRACE[2] = (u32)TimerRef;
        TRACE[3] = timer_ticks();
        TRACE[4] = NbFramePerSecond;
        TRACE[5] = video_vblanks();
        TRACE[6] = irq_total();
        TRACE[7] = irq_spurious();
        TRACE[8] = pad->buttons;
        TRACE[9] = hs_input_timeouts();
    }
    return 0;
}
