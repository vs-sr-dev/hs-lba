/*
 * platform_gfx.c — LIB_SVGA, the engine's graphics floor.
 *
 * The DOS original drew into `Log`, a 640x480 8bpp buffer in main memory, and
 * pushed it to `Phys`, the live VGA aperture, with S_PHYS.ASM. Everything the
 * engine renders — bricks, actors, menus, text — lands in Log through the
 * translated blitters in translate/, so the whole platform obligation is: own
 * Log, own the palette, and get rectangles of Log onto the television.
 *
 * Presentation is deliberately single-buffered. The engine's model is that Phys
 * *is* the screen: CopyBlockPhys() updates just the rectangle that changed, and
 * that is the entire basis of the incremental frame that makes LBA playable on
 * slow hardware. Flipping between two buffers would make every partial update
 * land on a surface that is about to be replaced by a stale one, so the
 * conversion writes into the buffer the TVE is currently scanning out. This is
 * what the DOS version did to a CRT, and at the frame rates in prospect there
 * is nothing to gain from double buffering it.
 *
 * This file also owns the engine globals that LIB_SVGA.ASM used to define, and
 * for that reason it does NOT include LIB_SVGA.H: that header declares
 * TabOffLine as a scalar, matching how the assembly took its address and
 * indexed off it, while the real object is an array. Keeping the two views in
 * separate translation units is the same trick the DS port uses.
 */

#include "video.h"
#include "memmap.h"

#include <stdlib.h>
#include <string.h>

typedef unsigned char  UBYTE;
typedef unsigned short UWORD;
typedef signed short   WORD;
typedef unsigned long  ULONG;
typedef signed long    LONG;

/* ---- the globals LIB_SVGA.ASM defined ---------------------------------- */

UBYTE *Log;                     /* 8bpp back buffer the engine renders into */
UBYTE *MemoLog;                 /* what to free: Log itself is moved around */
UBYTE *Phys;                    /* only the MCGA path writes here           */

WORD Screen_X = 640;            /* LBA1's CD build is natively 640x480      */
WORD Screen_Y = 480;

WORD ClipXmin, ClipYmin, ClipXmax, ClipYmax;
static WORD MemoClipXmin, MemoClipYmin, MemoClipXmax, MemoClipYmax;

UBYTE Text_Ink = 15;
UBYTE Text_Paper = (UBYTE)-1;
UBYTE OldVideo = (UBYTE)-1;
WORD  SizeCar = 8;

WORD  Svga_Card = 0;            /* SVGA_VESA */
void *BankChange = 0;
LONG  BankCurrent = -1;

ULONG TabOffLine[481];          /* per-line byte offsets into Log */

/* Palette: 0..255 per gun as the engine supplies it, kept alongside the RGB565
 * lookup the conversion actually uses. */
static UBYTE  PalRGB[768];
static unsigned short Pal565[256];

/*
 * On DOS the palette lived in the VGA DAC, downstream of the framebuffer: a
 * write to it restyled the pixels already on screen, instantly and for free.
 * That is the entire mechanism behind LBA's fades — FadeToPal() and friends
 * (AMBIANCE.C) loop `Vsync(); Palette(workpal);` and never touch a pixel.
 *
 * Here the palette is applied at conversion time, so a palette write changes
 * nothing that has already been converted to RGB565. Every fade would end with
 * the screen still showing whatever the palette was when it was last drawn —
 * which after a FadeToBlack is black, so the whole backdrop stays invisible
 * and only the rectangles the menu repaints afterwards appear.
 *
 * So a palette write marks the screen stale, and the next Vsync/present
 * reconverts the whole thing from Log. Deferring it to Vsync matters:
 * SetBlackPal() alone calls PalOne() 256 times, and a per-call repaint would
 * be 256 full-screen conversions.
 */
static int PalDirty;
static int McgaMode;            /* the F12 zoom: Phys, not Log, is the source */

/*
 * Present accounting, for phase E.
 *
 * The question the whole port turns on is whether the software rasteriser
 * keeps up, and the two halves of that are how often the engine finishes a
 * frame and how much of the screen it repaints to do it. LBA's frame is
 * incremental — FLIPBOX.C pushes a list of dirty rectangles — so "frames per
 * second" on its own says nothing without the area behind it.
 *
 * The frame count is the engine's own CmptFrame (PERSO.C, one per MainLoop
 * iteration); these two are the half it cannot see.
 */
unsigned int PORT_rects;        /* rectangles pushed to the screen */
/* Whole-screen presents. The FLA player is the reason: it never goes through
 * Flip() or Vsync(), so PORT_rects says nothing about a movie, and its pacing
 * loop waits on whole 50 Hz ticks (50/ImageCadence, four of them at cadence
 * 12). That quantisation is what makes a movie judder for a few per cent of
 * extra CPU rather than slow down smoothly, so the number worth watching is
 * how many frames a second actually reach the screen. */
unsigned int PORT_presents;
unsigned int PORT_pixels;       /* destination pixels written */

/*
 * Frame profile.
 *
 * PORT_rects/PORT_pixels alone cannot say whether the engine is *behind*. LBA's
 * MainLoop waits for its tick before starting the next frame, so a frame that
 * finishes early is invisible from outside — the loop just sits in the spin —
 * while a frame that overruns is equally invisible, because the wait is skipped
 * and nothing records that it was. The two look identical in a frame count.
 *
 * PORT_frames_idle is the one that separates them: it counts frames that
 * arrived at the regulator with time still on the clock. `idle / frames` is
 * therefore the headroom, directly, and 0% means the frame rate on screen is
 * the machine's ceiling rather than the regulator's.
 *
 * The rest are the work behind a frame, in the units that actually scale:
 * pixels restored from the clean background, and the screen area the 3D
 * objects covered. Both are counted at the engine's 640x480 resolution, which
 * is the resolution the rasterisers run at whatever the television gets.
 */
unsigned int PORT_frames;       /* MainLoop iterations                       */
unsigned int PORT_frames_idle;  /* ...that reached the tick regulator early  */
unsigned int PORT_cls_pixels;   /* Log pixels restored from Screen (ClsBoxes)*/
unsigned int PORT_objs;         /* 3D objects that rasterised something      */
unsigned int PORT_obj_pixels;   /* sum of their screen bounding boxes        */

/* ---- palette ----------------------------------------------------------- */

/* The VGA DAC only kept the top 6 bits, and LBA's art was authored against
 * that, so the truncation is part of the intended look rather than an
 * artefact to be avoided. */
static UBYTE Dac6(UBYTE v)
{
    UBYTE v6 = (UBYTE)(v >> 2);
    return (UBYTE)((v6 << 2) | (v6 >> 4));
}

static void PalApply(int start, int count)
{
    const UBYTE *p = PalRGB + start * 3;
    int i;

    for (i = start; i < start + count; i++, p += 3)
        Pal565[i] = (unsigned short)(((p[0] & 0xf8) << 8)
                                   | ((p[1] & 0xfc) << 3)
                                   | ( p[2] >> 3));

    PalDirty = 1;
}

void Palette(void *pal)
{
    const UBYTE *src = (const UBYTE *)pal;
    int i;

    for (i = 0; i < 768; i++)
        PalRGB[i] = Dac6(src[i]);
    PalApply(0, 256);
}

void PalMulti(WORD startcoul, WORD nbcoul, UBYTE *pal)
{
    UBYTE *dst = PalRGB + startcoul * 3;
    int i;

    for (i = 0; i < nbcoul * 3; i++)
        dst[i] = Dac6(pal[i]);
    PalApply(startcoul, nbcoul);
}

void PalOne(UBYTE coul, UBYTE r, UBYTE v, UBYTE b)
{
    UBYTE *dst = PalRGB + coul * 3;

    dst[0] = Dac6(r);
    dst[1] = Dac6(v);
    dst[2] = Dac6(b);
    PalApply(coul, 1);
}

/* ---- mode setup -------------------------------------------------------- */

static void BuildTabOffLine(void)
{
    int i;
    ULONG off = 0;

    for (i = 0; i < 481; i++, off += (ULONG)Screen_X)
        TabOffLine[i] = (i < Screen_Y) ? off : 0;
}

void *Malloc(LONG);             /* engine LIB_SYS/MALLOC.C */
void  Free(void *);

static void SetMode(int w, int h)
{
    Screen_X = (WORD)w;
    Screen_Y = (WORD)h;
    BuildTabOffLine();
    ClipXmin = 0;
    ClipYmin = 0;
    ClipXmax = (WORD)(w - 1);
    ClipYmax = (WORD)(h - 1);
    BankCurrent = -1;
}

/*
 * MCGA here is not a screen mode the engine renders in — it is LBA's F12 zoom,
 * and it only changes what reaches the television. The engine keeps drawing
 * into the same 640x480 Log: CopyBlockMCGA() (FUNC.C) advances its source by a
 * hardcoded `640 - width` per row and indexes TabOffLine to find the first one,
 * so both have to stay at the SVGA stride or the composition reads garbage.
 * What changes is that a 320x200 window of Log is composed into Phys and that
 * window, rather than all of Log, is what gets presented.
 */
void InitMcgaMode(void)
{
    if (!Phys)
        Phys = (UBYTE *)Malloc(64000L);
    if (Phys)
        memset(Phys, 0, 64000);
    McgaMode = 1;
#ifndef HS_VIDEO_FIXED_RES
    /* The source is about to be a 320x200 buffer, so scanning out at 640x480
     * would cost four times the present for detail that is not in it. */
    video_set_res(0);
#endif
}

void SimpleInitSvga(void)
{
    McgaMode = 0;
#ifndef HS_VIDEO_FIXED_RES
    video_set_res(1);
#endif
    SetMode(640, 480);
}

void InitGraphSvga(void)
{
    if (!Phys)
        Phys = (UBYTE *)Malloc(64000L);
    McgaMode = 0;
#ifndef HS_VIDEO_FIXED_RES
    video_set_res(1);
#endif
    SetMode(640, 480);
    Log = (UBYTE *)Malloc(640L * 480L);
    MemoLog = Log;
}

void ClearGraphSvga(void)
{
    Free(MemoLog);
    Log = 0;
    MemoLog = 0;
}

void InitGraphMcga(void)
{
    InitMcgaMode();
    Log = (UBYTE *)Malloc(640L * 480L);
    MemoLog = Log;
}

void ClearGraphMcga(void)
{
    Free(MemoLog);
    Log = 0;
    MemoLog = 0;
}

/* S_DLL.C: the DOS build could load a vendor SVGA driver. There is one video
 * chip here and we are already talking to it. */
LONG SvgaInitDLL(char *driverpathname)
{
    (void)driverpathname;
    return 1;
}

/* ---- clipping (INITSVGA.ASM) ------------------------------------------- */

void SetClip(LONG x0, LONG y0, LONG x1, LONG y1)
{
    ClipXmin = (WORD)((x0 < 0) ? 0 : x0);
    ClipYmin = (WORD)((y0 < 0) ? 0 : y0);
    ClipXmax = (WORD)((x1 >= Screen_X) ? Screen_X - 1 : x1);
    ClipYmax = (WORD)((y1 >= Screen_Y) ? Screen_Y - 1 : y1);
}

void UnSetClip(void)
{
    ClipXmin = 0;
    ClipYmin = 0;
    ClipXmax = (WORD)(Screen_X - 1);
    ClipYmax = (WORD)(Screen_Y - 1);
}

void MemoClip(void)
{
    MemoClipXmin = ClipXmin;
    MemoClipYmin = ClipYmin;
    MemoClipXmax = ClipXmax;
    MemoClipYmax = ClipYmax;
}

void RestoreClip(void)
{
    ClipXmin = MemoClipXmin;
    ClipYmin = MemoClipYmin;
    ClipXmax = MemoClipXmax;
    ClipYmax = MemoClipYmax;
}

/* ---- Log -> screen (S_PHYS.ASM) ---------------------------------------- */

/* One rectangle of Log, scaled to the 320x240 output: a 2:1 decimation in both
 * axes. */
static void present_rect(LONG x0, LONG y0, LONG x1, LONG y1)
{
    int outw, outh;

    if (!Log)
        return;

    video_get_res(&outw, &outh);

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > Screen_X - 1) x1 = Screen_X - 1;
    if (y1 > Screen_Y - 1) y1 = Screen_Y - 1;
    if (x1 < x0 || y1 < y0)
        return;

    PORT_rects++;
    /* Destination pixels, so the number means the same thing in both output
     * modes: at 320x240 that is a quarter of the source rectangle, at
     * 640x480 it is all of it. Hardcoding the >> 2 made the counter under-report
     * by 4x the moment HS_VIDEO_640 was tried. */
    PORT_pixels += (unsigned int)((x1 - x0 + 1) * (y1 - y0 + 1))
                 / (unsigned int)((Screen_X / outw) * (Screen_Y / outh));

    video_blit_log(Log, Pal565, (int)Screen_X, (int)Screen_Y,
                   (int)x0, (int)y0, (int)x1, (int)y1);
}

/*
 * The MCGA paths write to Phys directly, because on DOS Phys *was* the live
 * VGA aperture at 0xA0000 — GAMEMENU.C's menu backdrop is drawn that way, and
 * so is the logo. Here Phys is an ordinary 64000-byte buffer, so anything that
 * lands in it has to be pushed to the screen explicitly or it is simply never
 * seen. That is what a menu with no background looks like.
 */
static void present_phys(void)
{
    if (Phys)
        video_blit_log(Phys, Pal565, 320, 200, 0, 0, 319, 199);
}

/* The DAC write: re-colour the pixels that are on screen. Deliberately NOT a
 * reconversion of Log — see the shadow buffer in src/video.c for why Log holds
 * more than the engine ever displays. */
static void pal_flush(void)
{
    if (!PalDirty)
        return;
    video_repaint(Pal565);
    PalDirty = 0;
}

/* Everything the engine considers the screen. Both sources cover the whole
 * output, so either one settles it. */
static void present_full(void)
{
    if (McgaMode)
        present_phys();
    else
        present_rect(0, 0, Screen_X - 1, Screen_Y - 1);
    PalDirty = 0;
}

void Vsync(void)
{
    video_wait_vblank();

    /* After the vblank edge rather than before it: the conversion writes into
     * the buffer the TVE is scanning out, so this is the moment that leaves
     * the most of the frame ahead of the beam. */
    pal_flush();
}

void Flip(void)
{
    present_full();
}

void FlipComp(void)
{
    Flip();
}

void CopyBlockPhys(LONG x0, LONG y0, LONG x1, LONG y1)
{
    /* The rectangle first, so the recolour that follows covers the rest of the
     * screen with this rectangle's new content already in it. */
    present_rect(x0, y0, x1, y1);
    pal_flush();
}

/*
 * CopyBlockMCGA() in GAMEMENU.C composes into Phys and never calls anything to
 * show it, because on DOS the write itself was the display. Called after that
 * composition to get it on screen.
 */
void PORT_PresentPhys(void)
{
    present_phys();
    PORT_presents++;
    PalDirty = 0;
}

void CopyBlockPhysClip(LONG x0, LONG y0, LONG x1, LONG y1)
{
    if (x0 < ClipXmin) x0 = ClipXmin;
    if (y0 < ClipYmin) y0 = ClipYmin;
    if (x1 > ClipXmax) x1 = ClipXmax;
    if (y1 > ClipYmax) y1 = ClipYmax;
    CopyBlockPhys(x0, y0, x1, y1);
}
