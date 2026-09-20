#ifndef HS_VIDEO_H
#define HS_VIDEO_H

/*
 * Video output: a double-buffered 320x240 RGB565 screen, flipped in vblank,
 * plus the path that gets the engine's 640x480 8bpp Log buffer onto it.
 */

/*
 * Output resolution.
 *
 * The engine renders at 640x480 whatever this says (LOG_W/LOG_H below; that is
 * what LBA1's CD build is), so at 320x240 three of every four rendered pixels
 * are sampled away in video_blit_log(). The TVE can scan out 640x480 —
 * RESOLUTION_640_480 in the SDK, `case 0x4: // VGA` in MAME's tve_control_w —
 * so this is a choice, not a limit.
 *
 * 640x480 is the default: measured, it costs 6.7% of the gameplay frame rate
 * (PROJECT.md), which is cheap for showing four times the detail that is being
 * rendered either way. Full-screen presents — FLA movies, menus, fades — do pay
 * the full 4x, and the intro takes roughly twice as long.
 *
 * The open question is real hardware, which is not available: 640x480 over NTSC
 * is 480i, and LBA's brick texture is exactly the high-frequency content that
 * makes interlace flicker, while composite luma bandwidth caps horizontal
 * detail near 300 pixels either way. -DHS_VIDEO_QVGA goes back to 320x240 for
 * that comparison when there is a console to make it on.
 */
#ifdef HS_VIDEO_QVGA
#define VIDEO_W 320
#define VIDEO_H 240
#else
#define VIDEO_W 640
#define VIDEO_H 480
#endif

/* Engine-side dimensions. LBA1's CD build renders natively at 640x480. */
#define LOG_W 640
#define LOG_H 480

void video_init(void);

/* The buffer to draw into. Changes after every video_flip(). */
unsigned short *video_back(void);

/* Show the back buffer. Waits for the next vblank first, unless the vblank
 * interrupt is not running, in which case it flips immediately rather than
 * spinning forever. */
void video_flip(void);

/* Frames the vblank interrupt has counted. */
unsigned int video_vblanks(void);

/* Run `fn` from the vblank interrupt. This exists for input: the engine's
 * Key/Joy/Fire globals were maintained by the DOS keyboard IRQ, so game code
 * spins on them without calling anything — GAMEMENU.C's DoGameMenu() has its
 * Vsync() commented out and waits on `while (Joy OR Fire OR Key)`. Polled from
 * the foreground those loops never end. */
void video_set_vblank_hook(void (*fn)(void));

/* 640x480 Log -> 320x240 back buffer, every other pixel, through a 256-entry
 * RGB565 palette. */
void video_present_log(const unsigned char *log, const unsigned short *pal);

/* Block until the next vblank interrupt, or give up after roughly a frame if
 * none is arriving. */
void video_wait_vblank(void);

/* One rectangle of an 8bpp source of the given dimensions, scaled to the
 * 320x240 output and written into the buffer currently on screen.
 *
 * Into the *displayed* buffer, not the back one, because the engine's model is
 * that the screen is a surface it patches in place: CopyBlockPhys() repaints
 * only what changed, which is the whole basis of its incremental frame. */
void video_blit_log(const unsigned char *log, const unsigned short *pal,
                    int src_w, int src_h,
                    int x0, int y0, int x1, int y1);

/* Re-colour whatever video_blit_log() last put on screen through a new
 * palette. This is the DAC write the engine's fades assume: it changes the
 * colours of the displayed pixels and nothing else. */
void video_repaint(const unsigned short *pal);

/* Switch the scanout between 640x480 (hi != 0) and 320x240.
 *
 * The engine does not always present a 640x480 picture: the FLA movies and the
 * MCGA menu backdrop are a 320x200 buffer, and scanning those out at 640x480
 * costs four times the present for no detail that exists. Driven from
 * platform_gfx.c, which is where the engine says which of the two it is in.
 *
 * Wipes the framebuffer and the shadow — their stride changes meaning. */
void video_set_res(int hi);
void video_get_res(int *w, int *h);

void video_clear(unsigned short colour);

#endif
