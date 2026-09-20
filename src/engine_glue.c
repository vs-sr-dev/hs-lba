/*
 * engine_glue.c — the engine globals the translated blitters expect.
 *
 * Deliberately does NOT include translate.h: that header declares TabOffLine
 * as a scalar (the blitters take its address and index off it, matching the
 * original ASM), while the real object is an array. Defining it here keeps the
 * two views in separate translation units, exactly as the DS port does.
 */

typedef unsigned long  ULONG;
typedef signed long    LONG;
typedef signed short   WORD;
typedef unsigned char  UBYTE;

UBYTE *Log;                 /* 8bpp back buffer the engine renders into */
WORD   Screen_X = 640;      /* LBA1's CD build is natively 640x480      */
WORD   Screen_Y = 480;

WORD ClipXmin, ClipYmin, ClipXmax, ClipYmax;

ULONG TabOffLine[481];      /* per-line byte offsets into Log */

UBYTE Text_Ink, Text_Paper;

void engine_glue_init(UBYTE *logbuf)
{
    int i;

    Log = logbuf;
    for (i = 0; i < 481; i++)
        TabOffLine[i] = (ULONG)((long)i * Screen_X);

    ClipXmin = 0;
    ClipYmin = 0;
    ClipXmax = (WORD)(Screen_X - 1);
    ClipYmax = (WORD)(Screen_Y - 1);
}
