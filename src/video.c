/*
 * video.c — double buffer and vblank sync.
 *
 * The TVE keeps three framebuffer start addresses and a select register that
 * picks which one is scanned out (P_TV_BUFFER_SEL; 3 means "no output"). That
 * makes flipping a single register write — the buffers stay where they are and
 * nothing is copied.
 *
 * The vblank interrupt comes from the PPU, not the TVE: the PPU raises it on
 * the screen's vblank edge and drives CPU line 53. Its IRQ registers are not
 * in SPG290_Registers.h, so the three offsets used here are named after MAME's
 * spg290_ppu.cpp, which decodes 0x80 as the enable, 0x84 as the write-1-to-
 * clear status, and 0x94 as a readable raster line counter.
 *
 * Note the PPU only *records* a vblank in its status register when the
 * corresponding enable bit is set — the same gating the I2C block turned out to
 * have in m5 — so the enable has to go on even though we service the interrupt
 * rather than poll it.
 */

#include <string.h>

#include "video.h"
#include "irq.h"
#include "memmap.h"
#include "SPG290_Constants.h"
#include "SPG290_Registers.h"
#include "TV/TV.h"

typedef volatile unsigned int vu32;

#define PPU_BASE        0x88010000u
#define P_PPU_IRQ_CTRL  (*(vu32 *)(PPU_BASE + 0x80))
#define P_PPU_IRQ_ACK   (*(vu32 *)(PPU_BASE + 0x84))
#define P_PPU_LINE      (*(vu32 *)(PPU_BASE + 0x94))

#define PPU_IRQ_VBLANK_START  0x01u
#define PPU_IRQ_VBLANK_END    0x02u

/* Two 320x240x16bpp buffers out of the three the memory map reserves, in the
 * uncached window above the heap ceiling — see src/memmap.h for why they are no
 * longer at 0xA0500000, which was inside the heap. */
/* At 640x480 there is only room for one, and the game only ever uses one — so
 * all three TVE start addresses point at it. The register that selects which is
 * scanned out then has nothing to choose between, which is correct: nothing in
 * the engine path calls video_flip(). */
#define FB0 (FB_BASE)
#if FB_COUNT >= 3
#define FB1 (FB_BASE + FB_BYTES)
#define FB2 (FB_BASE + 2u * FB_BYTES)
#else
#define FB1 FB0
#define FB2 FB0
#endif

static unsigned short *const fbs[2] = {
    (unsigned short *)FB0,
    (unsigned short *)FB1
};

#ifdef HS_VIDEO_QVGA
#define TV_RESOLUTION RESOLUTION_320_240
#else
#define TV_RESOLUTION RESOLUTION_640_480
#endif

static unsigned int back = 1;                 /* index being drawn into */
static unsigned int shown = 0;                /* index the TVE is scanning */
static volatile unsigned int vblanks = 0;

/*
 * The palette indices of what is actually on screen.
 *
 * The VGA DAC sat downstream of the framebuffer, so writing it re-coloured the
 * pixels that were displayed — and only those. That distinction matters:
 * LBA's Log buffer holds more than the engine ever shows. GAMEMENU.C's fire
 * (FIRE.C DoFire) paints the full 640-pixel width of its rows and then pushes
 * only the 550-wide menu box, leaving bands in Log that were never meant to be
 * seen. Recolouring by reconverting Log would put them on screen.
 *
 * So the conversion keeps the indices it wrote, and a palette change re-expands
 * this rather than the source. 75 KB, and it makes the repaint a flat lookup
 * with none of the scaling arithmetic.
 */
static unsigned char shadow[VIDEO_W * VIDEO_H];

/*
 * The live output size. VIDEO_W/VIDEO_H are the *maximum* — what the buffers
 * above are sized for — while these two are what the TVE is scanning right now.
 *
 * The reason to move between them is that not everything the engine presents is
 * a 640x480 picture. The FLA movies and the MCGA menu backdrop are a 320x200
 * `Phys` buffer, so scanning them out at 640x480 costs four times the present
 * and cannot show a single pixel more: there is none to show. The logos and the
 * main menu *are* 640x480 in Log, and dropping those would lose real detail —
 * menu text especially — so the switch follows the source, not the situation.
 */
static int vid_w = VIDEO_W;
static int vid_h = VIDEO_H;

/*
 * The mode bits, written directly rather than through TV_Init().
 *
 * TV_Init() would work — it *assigns* P_TV_MODE_CTRL rather than or-ing into it
 * — but it also reclears and reselects the buffers, and the resolution is only
 * two bits. The trap to avoid is the obvious shortcut: C_TV_QVGA_MODE is
 * 0x00000000, so `*P_TV_MODE_CTRL |= C_TV_QVGA_MODE` is a no-op and would leave
 * the VGA bit set with nothing to show for it. The field has to be cleared and
 * rewritten.
 */
#define TV_MODE_RES_MASK   0x0000000cu

/* Two RGB565 pixels in one store. may_alias because the framebuffer is also
 * addressed as unsigned short elsewhere in this file. */
typedef unsigned int __attribute__((__may_alias__)) FB_U32;

/* If the interrupt never arrives, flipping must not wedge the machine. Sized
 * well past one 16.7 ms frame at MAME's ~18M instructions/s. */
#define VBLANK_SPINS 2000000u

static void (*vblank_hook)(void);

static void vblank_isr(void)
{
    P_PPU_IRQ_ACK = PPU_IRQ_VBLANK_START;
    vblanks++;
    if (vblank_hook)
        vblank_hook();
}

void video_set_vblank_hook(void (*fn)(void))
{
    vblank_hook = fn;
}

/*
 * Switch the scanout between 640x480 and 320x240.
 *
 * Both the framebuffer and the shadow are wiped: their stride changes meaning
 * with the mode, so every pixel already in them is at the wrong address
 * afterwards. Without this a switch shows one frame of the previous picture
 * sheared, and — worse, because it lasts — video_repaint() would recolour that
 * shear rather than the new content.
 *
 * On real hardware this is the one thing here that MAME cannot be trusted on: an
 * NTSC television resyncs when the mode changes, so every switch is likely a
 * black flash or a roll of up to a second. MAME reconfigures its screen
 * instantly and shows nothing of the sort. If that turns out to be intolerable
 * on a console, -DHS_VIDEO_FIXED_RES pins the output and gives up the saving.
 */
void video_set_res(int hi)
{
    int w = hi ? 640 : 320;
    int h = hi ? 480 : 240;
    unsigned int mode;

    if (w == vid_w && h == vid_h)
        return;

    vid_w = w;
    vid_h = h;

    mode = *P_TV_MODE_CTRL & ~TV_MODE_RES_MASK;
    *P_TV_MODE_CTRL = mode | (hi ? C_TV_VGA_MODE : C_TV_QVGA_MODE);

    {
        unsigned short *d = fbs[shown];
        int n = w * h;
        while (n--)
            *d++ = 0;
    }
    {
        unsigned char *k = shadow;
        int n = w * h;
        while (n--)
            *k++ = 0;
    }
}

void video_get_res(int *w, int *h)
{
    *w = vid_w;
    *h = vid_h;
}

void video_init(void)
{
    TV_Init(TV_RESOLUTION, COLOR_RGB565, FB0, FB1, FB2);

    irq_set_handler(IRQ_PPU, vblank_isr);
    P_PPU_IRQ_ACK  = PPU_IRQ_VBLANK_START | PPU_IRQ_VBLANK_END;
    P_PPU_IRQ_CTRL = PPU_IRQ_VBLANK_START;

    back = 1;
    shown = 0;
    TV_Buffer_Sel(0);
}

unsigned short *video_back(void)
{
    return fbs[back];
}

unsigned int video_vblanks(void)
{
    return vblanks;
}

void video_wait_vblank(void)
{
    unsigned int start = vblanks;
    unsigned int spins = VBLANK_SPINS;

    while (vblanks == start) {
        if (--spins == 0)
            return;             /* no vblank interrupt: do not wedge */
    }
}

void video_flip(void)
{
    video_wait_vblank();
    TV_Buffer_Sel(back);
    shown = back;
    back ^= 1;
}

/*
 * Scale one rectangle of an 8bpp source into the displayed buffer.
 *
 * The source is whatever the engine has set up — 640x480 for the CD build,
 * 320x200 in the MCGA paths — so the step is computed rather than assumed. A
 * 16.16 fixed-point accumulator keeps the inner loop to an add and a shift;
 * this runs over every pixel the engine repaints, so it is on the hot path for
 * every frame.
 */
void video_blit_log(const unsigned char *log, const unsigned short *pal,
                    int src_w, int src_h,
                    int x0, int y0, int x1, int y1)
{
    unsigned short *fb = fbs[shown];
    unsigned int step_x, step_y;
    int dx0, dy0, dx1, dy1, dy, prev_row;

    if (src_w <= 0 || src_h <= 0)
        return;

    step_x = ((unsigned int)src_w << 16) / vid_w;
    step_y = ((unsigned int)src_h << 16) / vid_h;

    /*
     * Source rectangle -> destination rectangle, rounding outwards: the low
     * edge down, the high edge UP.
     *
     * Truncating the high edge was the bug behind the few pixels of trail
     * Twinsen dragged behind him. At 2:1, a dirty box ending on an even source
     * column x1 gives (x1+1)/2 == x1/2, so the destination pixel that column
     * lands in is one short of the loop bound and never gets rewritten — the
     * engine repainted it in Log and the screen kept the old one. Rounding up
     * costs at most one extra destination column, taken from Log, which is
     * correct there anyway.
     */
    dx0 = (int)(((unsigned int)x0 << 16) / step_x);
    dy0 = (int)(((unsigned int)y0 << 16) / step_y);
    dx1 = (int)((((unsigned int)(x1 + 1) << 16) + step_x - 1) / step_x);
    dy1 = (int)((((unsigned int)(y1 + 1) << 16) + step_y - 1) / step_y);

    if (dx0 < 0) dx0 = 0;
    if (dy0 < 0) dy0 = 0;
    if (dx1 > vid_w) dx1 = vid_w;
    if (dy1 > vid_h) dy1 = vid_h;
    if (dx1 <= dx0 || dy1 <= dy0)
        return;

    /*
     * 1:1 is not a special case of scaling here, it is the common case: at
     * 640x480 output the source and destination are the same grid, and the
     * accumulator below would add and shift once per pixel to reproduce
     * `s[dxx]`. This loop runs over every pixel the engine repaints, so that
     * arithmetic is the difference between the present costing 8.8% of the
     * frame and costing what a copy costs.
     */
    if (step_x == (1u << 16) && step_y == (1u << 16)) {
        for (dy = dy0; dy < dy1; dy++) {
            const unsigned char *s = log + (unsigned int)dy * src_w + dx0;
            unsigned short *d = fb + dy * vid_w + dx0;
            unsigned char *k = shadow + dy * vid_w + dx0;
            int n = dx1 - dx0;

            while (n >= 4) {
                unsigned char c0 = s[0], c1 = s[1], c2 = s[2], c3 = s[3];

                k[0] = c0; k[1] = c1; k[2] = c2; k[3] = c3;
                d[0] = pal[c0]; d[1] = pal[c1];
                d[2] = pal[c2]; d[3] = pal[c3];
                s += 4; k += 4; d += 4; n -= 4;
            }
            while (n--) {
                unsigned char c = *s++;
                *k++ = c;
                *d++ = pal[c];
            }
        }
        return;
    }

    /*
     * Scaling path. The row cache is what makes it affordable when the source
     * is smaller than the screen, which since the output went to 640x480 is
     * every full-screen present the game makes: the FLA movies and the MCGA
     * menu backdrop are a 320x200 Phys buffer stretched over 640x480, so 280 of
     * the 480 destination rows repeat the row above them exactly. Converting
     * those again would be 180,000 palette lookups per movie frame for a result
     * already sitting one row up.
     */
    prev_row = -1;
    for (dy = dy0; dy < dy1; dy++) {
        int srow = (int)((unsigned int)dy * step_y >> 16);
        const unsigned char *s = log + (unsigned int)srow * src_w;
        unsigned short *d = fb + dy * vid_w + dx0;
        unsigned char *k = shadow + dy * vid_w + dx0;
        unsigned int sx = (unsigned int)dx0 * step_x;
        int dxx;

        if (srow == prev_row) {
            memcpy(d, d - vid_w, (size_t)(dx1 - dx0) * sizeof(*d));
            memcpy(k, k - vid_w, (size_t)(dx1 - dx0));
            continue;
        }
        prev_row = srow;

        for (dxx = dx0; dxx < dx1; dxx++) {
            unsigned char c = s[sx >> 16];
            *k++ = c;
            *d++ = pal[c];
            sx += step_x;
        }
    }
}

/*
 * Re-colour the screen through a new palette, leaving the picture alone.
 *
 * This is the whole screen every time, and no dirty-rectangle scheme can make
 * it less: a palette write changes every pixel that uses the changed entries,
 * and the engine does not know or record which those are. It is also not a rare
 * event — LBA's fades ARE this function. FadeToPal()/FadeToBlack() (AMBIANCE.C)
 * loop `Vsync(); Palette(workpal);` and never touch a pixel, because on DOS the
 * palette lived downstream of the framebuffer in the VGA DAC and recolouring was
 * free. Every logo in the intro fades in and out, so every step of every fade
 * lands here, and at 640x480 that is 307,200 pixels instead of 76,800.
 *
 * So it is written for throughput rather than clarity. Two pixels are packed
 * into one 32-bit store, which halves the store count and the loop overhead;
 * both buffers are 4-byte aligned and the pixel count is even, so the pairing
 * never needs a head or tail case. Little-endian, so the first pixel is the low
 * half.
 *
 * It measured at 29.4% of the pre-gameplay path before this
 * (tools/m21_profile.lua with HSLBA_PROF_INTRO=1).
 */
void video_repaint(const unsigned short *pal)
{
    FB_U32 *d = (FB_U32 *)fbs[shown];
    const unsigned char *s = shadow;
    int n = (vid_w * vid_h) / 2;

    while (n >= 4) {
        FB_U32 p0 = (FB_U32)pal[s[0]] | ((FB_U32)pal[s[1]] << 16);
        FB_U32 p1 = (FB_U32)pal[s[2]] | ((FB_U32)pal[s[3]] << 16);
        FB_U32 p2 = (FB_U32)pal[s[4]] | ((FB_U32)pal[s[5]] << 16);
        FB_U32 p3 = (FB_U32)pal[s[6]] | ((FB_U32)pal[s[7]] << 16);

        d[0] = p0; d[1] = p1; d[2] = p2; d[3] = p3;
        d += 4; s += 8; n -= 4;
    }
    while (n--) {
        *d++ = (FB_U32)pal[s[0]] | ((FB_U32)pal[s[1]] << 16);
        s += 2;
    }
}

void video_clear(unsigned short colour)
{
    unsigned short *d = fbs[back];
    int n = vid_w * vid_h;

    while (n--)
        *d++ = colour;
}

void video_present_log(const unsigned char *log, const unsigned short *pal)
{
    unsigned short *d = fbs[back];
    int y, x;

    for (y = 0; y < vid_h; y++) {
        const unsigned char *s = log + (y * 2) * LOG_W;
        for (x = 0; x < vid_w; x++)
            d[x] = pal[s[x * 2]];
        d += vid_w;
    }
}
