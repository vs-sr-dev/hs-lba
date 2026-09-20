/*
 * HS-LBA — milestone m5: the controller.
 *
 * Last missing piece before anything can be interactive. The SDK's
 * HS_Controller_Init() hangs on the first byte (see src/input.c for why), so
 * the I2C master is driven directly here.
 *
 * This is a live viewer: the five report bytes as they come off the bus, the
 * decoded buttons, the stick as numbers and as a dot in a box, and the
 * transfer/timeout counters. Everything it shows is also mirrored into the
 * TRACE block so a MAME Lua script can inject known inputs and assert on what
 * the driver actually saw, rather than on what a screenshot looks like.
 */

#include "TV/TV.h"
#include "SPG290_Registers.h"
#include "SPG290_Constants.h"
#include "input.h"

#include <stdio.h>
#include <string.h>

#define SCR_W 320
#define SCR_H 240

#define FB0 0xA0500000u
#define FB1 (FB0 + 0x00040000u)
#define FB2 (FB0 + 0x00080000u)

/* Mirror of the driver state, for the Lua harness:
 *   [0] milestone marker      [4] raw[0..3] packed
 *   [1] polls completed       [5] raw[4] (checksum byte)
 *   [2] buttons               [6] ax (signed)
 *   [3] dpad                  [7] ay (signed)
 *   [8] transfers             [9] timeouts                             */
#define TRACE ((volatile unsigned int *)0xA0A00000u)
#define MARK(n) do { TRACE[0] = 0xAA000000u | (n); } while (0)

typedef unsigned int u32;
typedef unsigned char u8;

/* Each timed phase is sized to run for a couple of seconds, long enough that a
 * sampler attaching at an arbitrary moment still gets a clean slope out of it. */
#define BENCH_SINGLE_N 5000     /* one-byte transfers   */
#define BENCH_FULL_N   1000     /* five-byte pad reads  */

extern const unsigned char font[];

#define GREEN 0x07E0
#define RED   0xF800
#define WHITE 0xFFFF
#define GREY  0x8410
#define BLUE  0x001F
#define YEL   0xFFE0
#define BG    0x0000

static void hs_puts(unsigned short *fb, int cx, int cy, const char *s,
                    unsigned short fg, unsigned short bg)
{
    while (*s) {
        const unsigned char *g = &font[((unsigned char)*s) * 16];
        int yy;
        for (yy = 0; yy < 16; yy++) {
            unsigned short *d = fb + (cy * 16 + yy) * SCR_W + cx * 8;
            unsigned char row = g[yy];
            int xx;
            for (xx = 0; xx < 8; xx++)
                d[xx] = (row & (0x80 >> xx)) ? fg : bg;
        }
        cx++;
        s++;
    }
}

static void clear(unsigned short *fb, unsigned short c)
{
    int n = SCR_W * SCR_H;
    while (n--)
        *fb++ = c;
}

static void box(unsigned short *fb, int x0, int y0, int w, int h,
                unsigned short c)
{
    int i;
    for (i = 0; i < w; i++) {
        fb[y0 * SCR_W + x0 + i] = c;
        fb[(y0 + h - 1) * SCR_W + x0 + i] = c;
    }
    for (i = 0; i < h; i++) {
        fb[(y0 + i) * SCR_W + x0] = c;
        fb[(y0 + i) * SCR_W + x0 + w - 1] = c;
    }
}

static void fill(unsigned short *fb, int x0, int y0, int w, int h,
                 unsigned short c)
{
    int y, x;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            fb[(y0 + y) * SCR_W + x0 + x] = c;
}

/* One label per button, drawn lit when held. */
struct btn { const char *name; unsigned int mask; unsigned short lit; };

static const struct btn buttons[] = {
    { "GRN",  HS_BTN_GREEN,  GREEN },
    { "RED",  HS_BTN_RED,    RED   },
    { "YEL",  HS_BTN_YELLOW, YEL   },
    { "BLU",  HS_BTN_BLUE,   BLUE  },
    { "STA",  HS_BTN_START,  WHITE },
    { "SEL",  HS_BTN_SELECT, WHITE },
    { "LS",   HS_BTN_LS,     WHITE },
    { "RS",   HS_BTN_RS,     WHITE },
    { "LT",   HS_BTN_LT,     WHITE },
    { "RT",   HS_BTN_RT,     WHITE },
};
#define NBTN (int)(sizeof(buttons) / sizeof(buttons[0]))

static const char *dpad_str(unsigned int d)
{
    static char s[6];
    s[0] = (d & HS_DPAD_UP)    ? 'U' : '.';
    s[1] = (d & HS_DPAD_DOWN)  ? 'D' : '.';
    s[2] = (d & HS_DPAD_LEFT)  ? 'L' : '.';
    s[3] = (d & HS_DPAD_RIGHT) ? 'R' : '.';
    s[4] = 0;
    return s;
}

#define STICK_X 224
#define STICK_Y 96
#define STICK_S 72

static void draw(unsigned short *fb, const hs_pad *p, u32 polls)
{
    char line[64];
    int i, dx, dy;

    sprintf(line, "raw %02X %02X %02X %02X  chk %02X",
            p->raw[0], p->raw[1], p->raw[2], p->raw[3], p->raw[4]);
    hs_puts(fb, 1, 2, line, GREY, BG);

    sprintf(line, "btn %04X  dpad %s  %s",
            p->buttons, dpad_str(p->dpad), p->ok ? "ok " : "ERR");
    hs_puts(fb, 1, 3, line, p->ok ? GREY : RED, BG);

    for (i = 0; i < NBTN; i++) {
        int held = (p->buttons & buttons[i].mask) != 0;
        int cx = 1 + (i % 5) * 4;
        int cy = 5 + (i / 5);
        hs_puts(fb, cx, cy, buttons[i].name,
                held ? BG : GREY, held ? buttons[i].lit : BG);
    }

    sprintf(line, "ax %+4d  ay %+4d ", p->ax, p->ay);
    hs_puts(fb, 1, 8, line, WHITE, BG);

    sprintf(line, "polls %u  xfer %u  to %u   ",
            polls, hs_input_transfers(), hs_input_timeouts());
    hs_puts(fb, 1, 13, line,
            hs_input_timeouts() ? RED : GREY, BG);

    /* Stick box: dot at the current position, cross at rest. */
    fill(fb, STICK_X, STICK_Y, STICK_S, STICK_S, BG);
    box(fb, STICK_X, STICK_Y, STICK_S, STICK_S, GREY);
    dx = STICK_X + STICK_S / 2 + (p->ax * (STICK_S / 2 - 3)) / 128;
    dy = STICK_Y + STICK_S / 2 + (p->ay * (STICK_S / 2 - 3)) / 128;
    fill(fb, dx - 2, dy - 2, 5, 5, p->dpad ? GREEN : WHITE);
}

int main(void)
{
    unsigned short *fb = (unsigned short *)FB0;
    const hs_pad *p;
    u32 polls = 0;

    MARK(1);
    TV_Init(RESOLUTION_320_240, COLOR_RGB565, FB0, FB1, FB2);
    clear(fb, BG);
    hs_puts(fb, 1, 0, "HS-LBA  m5  controller", WHITE, BG);

    if (hs_input_init() < 0) {
        /* A dead bus must not turn the timed phases into a 50-second stall of
         * back-to-back timeouts. */
        hs_puts(fb, 1, 2, "I2C timeout - no pad", RED, BG);
        MARK(0xFF);
        for (;;) { }
    }

    p = hs_input_pad();

    /* Two timed phases before the viewer, so the bus cost is a measured
     * number and not an estimate off MAME's clock model. The I2C block runs on
     * wall-clock time, unlike the CPU that MAME slows by ~6x, so whatever this
     * costs here it costs on a real console too — and as a *share* of a frame
     * it gets worse there, not better. */
    TRACE[1] = 0;
    MARK(3);
    for (polls = 0; polls < BENCH_SINGLE_N; polls++) {
        unsigned int v;
        hs_input_read_raw(0, &v);
        TRACE[1] = polls + 1;
    }

    TRACE[1] = 0;
    MARK(4);
    for (polls = 0; polls < BENCH_FULL_N; polls++) {
        hs_input_poll();
        TRACE[1] = polls + 1;
    }

    MARK(2);
    TRACE[1] = 0;
    polls = 0;

    for (;;) {
        hs_input_poll();
        polls++;

        draw(fb, p, polls);

        TRACE[1] = polls;
        TRACE[2] = p->buttons;
        TRACE[3] = p->dpad;
        TRACE[4] = (u32)p->raw[0] | ((u32)p->raw[1] << 8)
                 | ((u32)p->raw[2] << 16) | ((u32)p->raw[3] << 24);
        TRACE[5] = p->raw[4];
        TRACE[6] = (u32)p->ax;
        TRACE[7] = (u32)p->ay;
        TRACE[8] = hs_input_transfers();
        TRACE[9] = hs_input_timeouts();
    }
    return 0;
}
