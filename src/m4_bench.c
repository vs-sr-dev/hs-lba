/*
 * HS-LBA — milestone m4: real brick blitting, and the measurement that
 * decides the project.
 *
 * Runs the engine's actual RLE brick blitter (translate/graph_a.c, the C
 * translation of Adeline's GRAPH_A.ASM) over real bricks from LBA_BRK.HQR,
 * into a real 640x480 8bpp Log buffer — full resolution, no shortcuts. It then
 * covers the screen over and over and counts completed passes.
 *
 * MAME supplies the time base: run for a known number of emulated milliseconds
 * with `gtime`, read the pass counter out of memory, and the cost per brick
 * falls out. The SPG290 timer device is no help here — it never exposes its
 * counter register for reading.
 *
 * Note this measures the *dominant term* of AffGrille (the brick blits), not
 * AffGrille itself: no cube walk, no depth overdraw. It is therefore a lower
 * bound on a full redraw, which is what we want from a first number.
 */

#include "TV/TV.h"
#include "SPG290_Registers.h"
#include "SPG290_Constants.h"
#include "cd.h"
#include "hqr.h"

#include <stdio.h>
#include <string.h>

#define SCR_W 320
#define SCR_H 240

#define FB0 0xA0500000u
#define FB1 (FB0 + 0x00040000u)
#define FB2 (FB0 + 0x00080000u)

/* [0] milestone marker, [1] passes completed, [2] bricks per pass */
#define TRACE ((volatile unsigned int *)0xA0A00000u)
#define MARK(n) do { TRACE[0] = 0xAA000000u | (n); } while (0)

#define BRK_SECTORS  300             /* offset table + the first few hundred bricks */
#define NBRICK       128
#define BANK_BYTES   (NBRICK * 1600)

#define BRICK_STEP_X 48              /* natural brick width  */
#define BRICK_STEP_Y 24              /* isometric rows overlap vertically */

typedef unsigned int u32;
typedef unsigned char u8;

struct hs_entry { char name[16]; u32 lba; u32 size; u32 sum; };
struct hs_header { char magic[8]; u32 count; u32 pad; };

static u8 index_sector[2048];
static u8 ress_head[2048];               /* RESS offset table + entry 0 (palette) */
static u8 brk[BRK_SECTORS * 2048];
static u8 bank[BANK_BYTES];
static u8 logbuf[640 * 480];
static unsigned short pal565[256];

extern const unsigned char font[];

/* from engine_glue.c */
extern void engine_glue_init(u8 *logbuf);
/* from translate/graph_a.c — the engine's own blitter, unmodified */
extern void AffGraph(long num, long x, long y, void *bank);

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

#define GREEN 0x07E0
#define RED   0xF800
#define WHITE 0xFFFF
#define GREY  0xC618
#define BG    0x0000

static void die(unsigned short *fb, const char *msg)
{
    hs_puts(fb, 1, 8, msg, RED, BG);
    for (;;) { }
}

static void build_palette(const u8 *p)
{
    int i, six = 1;

    for (i = 0; i < 768; i++)
        if (p[i] >= 64) { six = 0; break; }

    for (i = 0; i < 256; i++) {
        u8 r = p[i * 3 + 0], g = p[i * 3 + 1], b = p[i * 3 + 2];
        if (six) {
            r = (u8)((r << 2) | (r >> 4));
            g = (u8)((g << 2) | (g >> 4));
            b = (u8)((b << 2) | (b >> 4));
        }
        pal565[i] = RGB565(r, g, b);
    }
}

/* 640x480 Log -> 320x240 screen, sampling every other pixel (option B's
 * operation, applied after the fact here). */
static void present(unsigned short *fb)
{
    int y, x;
    for (y = 0; y < SCR_H; y++) {
        const u8 *s = logbuf + (y * 2) * 640;
        unsigned short *d = fb + y * SCR_W;
        for (x = 0; x < SCR_W; x++)
            d[x] = pal565[s[x * 2]];
    }
}

static const struct hs_entry *find_file(const struct hs_header *hdr,
                                        const struct hs_entry *ents,
                                        const char *name)
{
    int i;
    for (i = 0; i < (int)hdr->count; i++)
        if (strncmp(ents[i].name, name, 15) == 0)
            return &ents[i];
    return 0;
}

/* Build the brick bank AffGraph expects: a u32 offset table followed by the
 * decoded bricks. This is what GRILLE.C's LoadUsedBrick() produces per scene. */
static int build_bank(int want)
{
    u32 *table = (u32 *)bank;
    u32 pos = (u32)(want * 4);
    int got = 0;

    while (got < want) {
        u32 size = hqr_entry_size(brk, got);

        if (size == 0 || pos + size > BANK_BYTES)
            break;
        if (hqr_load(brk, got, bank + pos) != size)
            break;
        table[got] = pos;
        pos += size;
        got++;
    }
    return got;
}

/* One screen cover with real bricks. Returns the number of blits. */
static int draw_pass(int nbricks, int phase)
{
    int x, y, n = 0;

    for (y = 0; y < 480; y += BRICK_STEP_Y)
        for (x = 0; x < 640; x += BRICK_STEP_X) {
            AffGraph((long)((n + phase) % nbricks), x, y, bank);
            n++;
        }
    return n;
}

int main(void)
{
    unsigned short *fb = (unsigned short *)FB0;
    const struct hs_header *hdr;
    const struct hs_entry *ents;
    const struct hs_entry *e;
    char line[64];
    int nbricks, per_pass;
    u32 passes = 0;

    MARK(1);
    TV_Init(RESOLUTION_320_240, COLOR_RGB565, FB0, FB1, FB2);
    clear(fb, BG);
    hs_puts(fb, 1, 0, "HS-LBA  m4  brick blit", WHITE, BG);

    if (cd_init() < 97)
        die(fb, "no disc");
    if (cd_read(0, index_sector, 1) != 0)
        die(fb, "index read failed");

    hdr  = (const struct hs_header *)index_sector;
    ents = (const struct hs_entry *)(index_sector + sizeof(struct hs_header));
    if (memcmp(hdr->magic, "HSLBA1", 6) != 0)
        die(fb, "bad archive magic");
    MARK(2);

    /* Game palette: RESS.HQR entry 0, stored uncompressed and small enough
     * that the offset table and the palette both live in the first sector. */
    e = find_file(hdr, ents, "RESS.HQR");
    if (!e || cd_read(e->lba, ress_head, 1) != 0)
        die(fb, "RESS.HQR head read failed");
    {
        static u8 palbuf[1024];
        if (hqr_load(ress_head, 0, palbuf) != 768)
            die(fb, "palette decode failed");
        build_palette(palbuf);
    }
    MARK(3);

    e = find_file(hdr, ents, "LBA_BRK.HQR");
    if (!e)
        die(fb, "LBA_BRK.HQR not in archive");
    if (cd_read(e->lba, brk, BRK_SECTORS) != 0)
        die(fb, "LBA_BRK.HQR read timeout");
    MARK(4);

    nbricks = build_bank(NBRICK);
    sprintf(line, "%d bricks decoded", nbricks);
    hs_puts(fb, 1, 1, line, GREY, BG);
    if (nbricks < 8)
        die(fb, "too few bricks");
    MARK(5);

    engine_glue_init(logbuf);
    memset(logbuf, 0, sizeof(logbuf));

    /* One pass drawn and shown, so the blitter is visibly correct before the
     * numbers mean anything. */
    per_pass = draw_pass(nbricks, 0);
    present(fb);
    TRACE[2] = (unsigned)per_pass;
    MARK(6);

    sprintf(line, " %d blits/pass  timing... ", per_pass);
    hs_puts(fb, 0, 14, line, GREEN, BG);

    /* Timed section: nothing but real brick blits into the 640x480 Log. */
    for (;;) {
        draw_pass(nbricks, (int)passes);
        TRACE[1] = ++passes;
    }
    return 0;
}
