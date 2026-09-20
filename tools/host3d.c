/*
 * host3d.c — run the translated 3D renderer on the build machine.
 *
 * translate/p_ob_iso.c and p_trigo.c are portable C, so they can be compiled
 * for the host and fed a real object out of INVOBJ.HQR. The point is the
 * iteration time: a question about the projection maths answered here costs
 * two seconds, against roughly ten minutes for a cross-build plus a MAME run
 * driven by hand to the right moment in the game.
 *
 * What it reproduces is GAMEMENU.C's Draw3dObject():
 *
 *     SetProjection(x, y, 128, 200, 200);
 *     SetFollowCamera(0, 0, 0, 60, 0, 0, zoom);
 *     AffObjetIso(0, 0, 0, 0, beta, 0, ptrobj);
 *
 * — the *perspective* camera. The game world is isometric, so this path is
 * used by almost nothing: the inventory objects, the "found an object"
 * sequence, and the holomap. Which is exactly the set reported broken.
 *
 * The rasteriser is stubbed to counters. Nothing here needs pixels: the
 * question is whether the vertices land on the screen at all.
 *
 * Build (needs C:\msys64\mingw64\bin on PATH for the assembler):
 *   gcc -O1 -w -Itranslate -Icompat -o build/hosttest/host3d.exe \
 *       tools/host3d.c translate/p_ob_iso.c translate/p_trigo.c \
 *       engine/LIB_3D/P_SINTAB.C
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "translate.h"
#include "lib3d_p.h"

/*──────────────────────── rasteriser stubs ────────────────────────────────*/

WORD TabPoly[1000];
WORD NbPolyPoints;
WORD TypePoly;

static int n_poly, n_line, n_sphere, n_fill;

LONG ComputePoly_A(void) { n_poly++; return 0; }
LONG ComputeSphere_A(LONG xc, LONG yc, LONG rayon)
{
    (void)xc; (void)yc; (void)rayon;
    n_sphere++;
    return 0;
}
void FillVertic_A(LONG typepoly, LONG coulpoly)
{
    (void)typepoly; (void)coulpoly;
    n_fill++;
}
void Line_A(LONG x0, LONG y0, LONG x1, LONG y1, LONG coul)
{
    (void)x0; (void)y0; (void)x1; (void)y1; (void)coul;
    n_line++;
}

/*──────────────────────── HQR reading ─────────────────────────────────────*/

/* engine/LIB_SYS/EXPAND.C, transliterated to indices so it cannot walk off a
 * host heap the way the pointer version would if a size were wrong. */
static void expand(const unsigned char *src, unsigned char *dst, long count)
{
    long si = 0, di = 0, left = count;

    while (left > 0) {
        unsigned char flags = src[si++];
        int b;

        for (b = 0; b < 8; b++) {
            if (flags & 1) {
                dst[di++] = src[si++];
                if (--left == 0) return;
            } else {
                unsigned short ax = (unsigned short)(src[si] | (src[si + 1] << 8));
                int n = (ax & 0x0F) + 2, k;
                long back = (long)di - (long)((ax >> 4) + 1);

                si += 2;
                left -= n;
                for (k = 0; k < n; k++, di++)
                    dst[di] = dst[back + k];
                if (left <= 0) return;
            }
            flags >>= 1;
        }
    }
}

static unsigned char *hqr_get(const char *path, int index, unsigned long *outsize)
{
    FILE *f = fopen(path, "rb");
    unsigned char *file, *out;
    unsigned long len, tbl, off, size, szl;
    unsigned short meth;

    if (!f) { printf("cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
    file = malloc(len);
    if (fread(file, 1, len, f) != len) { printf("short read\n"); exit(1); }
    fclose(f);

    tbl = file[0] | (file[1] << 8) | (file[2] << 16) | ((unsigned long)file[3] << 24);
    if ((unsigned long)index >= tbl / 4 - 1) { printf("index out of range\n"); exit(1); }
    off = file[index*4] | (file[index*4+1] << 8) | (file[index*4+2] << 16)
        | ((unsigned long)file[index*4+3] << 24);

    memcpy(&size, file + off, 4);
    memcpy(&szl,  file + off + 4, 4);
    memcpy(&meth, file + off + 8, 2);

    out = malloc(size + 512);
    if (meth == 0) memcpy(out, file + off + 10, size);
    else           expand(file + off + 10, out, (long)size);

    free(file);
    *outsize = size;
    return out;
}

/*──────────────────────── the experiment ──────────────────────────────────*/

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "data/INVOBJ.HQR";
    int index = (argc > 2) ? atoi(argv[2]) : 0;
    int beta  = (argc > 3) ? atoi(argv[3]) : 0;
    int zoom  = (argc > 4) ? atoi(argv[4]) : 10000;
    unsigned long size;
    unsigned char *obj = hqr_get(path, index, &size);
    LONG ret;
    int i, onscreen = 0;
    WORD xmin = 32767, xmax = -32768, ymin = 32767, ymax = -32768;

    printf("%s[%d]: %lu bytes, flags=0x%04X %s\n", path, index, size,
           (unsigned)(obj[0] | (obj[1] << 8)),
           (obj[0] & INFO_ANIM) ? "ANIM" : "STATIC");

    /* GAMEMENU.C draws inventory objects into a 75x75-ish box centred on
     * (x, y); the numbers below are DoFoundObj's. */
    SetProjection(320, 240, 128, 200, 200);
    SetFollowCamera(0, 0, 0, 60, 0, 0, zoom);

    ScreenXmin = ScreenYmin = 32767;
    ScreenXmax = ScreenYmax = -32768;

    ret = AffObjetIso(0, 0, 0, 0, (WORD)beta, 0, (WORD *)obj);

    printf("AffObjetIso -> %ld (%s)\n", (long)ret,
           ret == 0 ? "something displayed" : "NOTHING displayed");
    printf("NbPoints=%d  screen box = (%d,%d)-(%d,%d)\n",
           (int)NbPoints, (int)ScreenXmin, (int)ScreenYmin,
           (int)ScreenXmax, (int)ScreenYmax);
    printf("rasteriser: polys=%d lines=%d spheres=%d fills=%d\n",
           n_poly, n_line, n_sphere, n_fill);

    for (i = 0; i < NbPoints; i++) {
        WORD x = List_Point[i*3 + 0], y = List_Point[i*3 + 1];
        if (x < xmin) xmin = x;
        if (x > xmax) xmax = x;
        if (y < ymin) ymin = y;
        if (y > ymax) ymax = y;
        if (x >= 0 && x < 640 && y >= 0 && y < 480) onscreen++;
    }
    printf("projected: x %d..%d  y %d..%d   %d/%d vertices inside 640x480\n",
           (int)xmin, (int)xmax, (int)ymin, (int)ymax, onscreen, (int)NbPoints);

    printf("first 8 vertices (Xp,Yp,Zsort):");
    for (i = 0; i < NbPoints && i < 8; i++)
        printf(" (%d,%d,%d)", List_Point[i*3], List_Point[i*3+1], List_Point[i*3+2]);
    printf("\n");

    return 0;
}
