/*
 * HS-LBA — milestone m7: fopen() works, and the heap is where we say it is.
 *
 * This closes the platform layer. The engine's file I/O all funnels through
 * LIB_SYS/FILES.C, which is plain stdio — fopen/fread/fseek/ftell — so rather
 * than rewrite it, newlib's bottom end now sits on the disc archive
 * (src/syscalls.c over src/fs.c) and FILES.C can be compiled untouched.
 *
 * The test is deliberately end-to-end and self-checking: read every file in
 * the archive through stdio, in awkwardly sized chunks that straddle sector
 * boundaries, and compare the running FNV-1a against the checksum tools/mkcd.py
 * recorded at build time. A read path that is off by a sector, loses the tail
 * of a file, or mishandles an unaligned seek cannot produce a matching hash.
 *
 * It also allocates, which nothing in this port had done before. See
 * src/memmap.h: the SDK's heap was an 8 MB array in .bss with the framebuffers
 * inside it, so the first program to malloc a few megabytes would have started
 * drawing on the screen.
 */

#include "TV/TV.h"
#include "irq.h"
#include "timer.h"
#include "video.h"
#include "input.h"
#include "fs.h"
#include "heap.h"
#include "memmap.h"
#include "trace.h"
#include "cd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*  [0] marker            [5] heap free (KB)
 *  [1] files checked     [6] heap used (KB)
 *  [2] files matching    [7] malloc test result
 *  [3] bytes read (KB)   [8] TimerRef
 *  [4] first mismatch    [9] seek test result
 * [10] CD seeks issued   [11] sectors delivered                            */

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
    while (*s && cx < 40) {
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

/* Deliberately not a power of two and not a sector multiple, so every file
 * exercises the head/bulk/tail split in fs_read() at shifting offsets. */
#define CHUNK 3000

static unsigned char chunk[CHUNK];

/* Read one file the way the engine would and hash what comes back. */
static int verify_file(int index, u32 *bytes_out)
{
    char path[32];
    FILE *fp;
    u32 sum = 2166136261u;
    u32 total = 0;
    size_t got;

    strcpy(path, fs_name(index));
    fp = fopen(path, "rb");
    if (!fp)
        return -1;

    while ((got = fread(chunk, 1, CHUNK, fp)) > 0) {
        u32 i;
        for (i = 0; i < (u32)got; i++) {
            sum ^= chunk[i];
            sum *= 16777619u;
        }
        total += (u32)got;
    }
    fclose(fp);

    *bytes_out = total;

    if (total != fs_entry_size(index))
        return -2;
    if (sum != fs_entry_sum(index))
        return -3;
    return 0;
}

/* ftell/fseek have to agree with the archive, or HQR entry lookup — which is
 * nothing but seek-to-offset — will read the wrong resource. */
static int verify_seek(const char *name)
{
    FILE *fp = fopen(name, "rb");
    unsigned char a[4], b[4];
    long size;

    if (!fp)
        return -1;

    fseek(fp, 0L, SEEK_END);
    size = ftell(fp);
    if (size < 2049L) {          /* needs to span a sector boundary to matter */
        fclose(fp);
        return -2;
    }

    /* Read four bytes at a deliberately unaligned offset, then come back to
     * them the long way round and check they are the same four bytes. */
    fseek(fp, 2045L, SEEK_SET);
    if (fread(a, 1, 4, fp) != 4) { fclose(fp); return -3; }

    fseek(fp, 0L, SEEK_SET);
    fseek(fp, 1000L, SEEK_CUR);
    fseek(fp, 1045L, SEEK_CUR);
    if (ftell(fp) != 2045L) { fclose(fp); return -4; }
    if (fread(b, 1, 4, fp) != 4) { fclose(fp); return -5; }

    fclose(fp);
    return memcmp(a, b, 4) == 0 ? 0 : -6;
}

/* Take a real bite out of the heap and prove it is where memmap.h says. */
static int verify_heap(void)
{
    const u32 want = 4u * 1024u * 1024u;
    unsigned char *p = (unsigned char *)malloc(want);
    u32 addr;

    if (!p)
        return -1;

    addr = (u32)p;
    p[0] = 0x5a;
    p[want - 1] = 0xa5;

    /* The old arrangement would have put this straight through the display. */
    if (addr < heap_base() || addr + want > HEAP_END) {
        free(p);
        return -2;
    }
    if (addr < FB_BASE && addr + want > FB_BASE) {
        free(p);
        return -3;
    }
    if (p[0] != 0x5a || p[want - 1] != 0xa5) {
        free(p);
        return -4;
    }

    free(p);
    return 0;
}

int main(void)
{
    unsigned short *fb;
    int i, n, matched = 0, first_bad = -1;
    int heap_rc, seek_rc;
    u32 total_kb = 0;
    char line[48];
    int row;

    MARK(1);
    irq_init();
    video_init();
    InitTimer();
    hs_input_init();

    fb = video_back();
    hs_puts(fb, 1, 0, "HS-LBA  m7  stdio + heap", WHITE, BG);
    hs_puts(fb, 1, 2, "mounting...", GREY, BG);
    video_flip();

    MARK(2);
    if (cd_init() < 97 || fs_mount() != 0) {
        fb = video_back();
        hs_puts(fb, 1, 2, "no archive on disc", RED, BG);
        video_flip();
        MARK(0xF7);
        for (;;) { }
    }

    n = fs_count();
    MARK(3);

    heap_rc = verify_heap();
    TRACE[7] = (u32)heap_rc;
    MARK(4);

    seek_rc = verify_seek("SCENE.HQR");
    TRACE[9] = (u32)seek_rc;
    MARK(5);

    for (i = 0; i < n; i++) {
        u32 bytes = 0;
        int rc = verify_file(i, &bytes);

        total_kb += bytes / 1024u;
        if (rc == 0)
            matched++;
        else if (first_bad < 0)
            first_bad = i;

        TRACE[1] = (u32)(i + 1);
        TRACE[2] = (u32)matched;
        TRACE[3] = total_kb;
        TRACE[4] = (u32)first_bad;

        /* Progress, so a long verify does not look like a hang. */
        fb = video_back();
        hs_puts(fb, 1, 0, "HS-LBA  m7  stdio + heap", WHITE, BG);
        sprintf(line, "%s %s   ", fs_name(i), rc == 0 ? "ok " : "BAD");
        hs_puts(fb, 1, 2, line, rc == 0 ? GREY : RED, BG);
        sprintf(line, "%d/%d verified  %u KB", matched, i + 1, total_kb);
        hs_puts(fb, 1, 3, line, GREY, BG);
        video_flip();
    }
    MARK(6);

    TRACE[5] = heap_free() / 1024u;
    TRACE[6] = heap_used() / 1024u;
    TRACE[10] = cd_seeks();
    TRACE[11] = cd_sectors_read();

    for (;;) {
        hs_input_poll();

        /* Both buffers still carry the progress text from the verify pass, and
         * a shorter line does not erase a longer one. */
        video_clear(BG);
        fb = video_back();
        row = 0;
        hs_puts(fb, 1, row++, "HS-LBA  m7  stdio + heap", WHITE, BG);
        row++;

        sprintf(line, "archive  %d files, %u KB", n, total_kb);
        hs_puts(fb, 1, row++, line, GREY, BG);

        sprintf(line, "checksums %d/%d", matched, n);
        hs_puts(fb, 1, row++, line, (matched == n) ? GREEN : RED, BG);

        sprintf(line, "fseek/ftell  %s", seek_rc == 0 ? "ok" : "FAIL");
        hs_puts(fb, 1, row++, line, seek_rc == 0 ? GREEN : RED, BG);

        sprintf(line, "malloc 4MB   %s", heap_rc == 0 ? "ok" : "FAIL");
        hs_puts(fb, 1, row++, line, heap_rc == 0 ? GREEN : RED, BG);
        row++;

        sprintf(line, "heap %08X..%08X", heap_base(), (unsigned)HEAP_END);
        hs_puts(fb, 1, row++, line, CYAN, BG);
        sprintf(line, "free %u KB", heap_free() / 1024u);
        hs_puts(fb, 1, row++, line, CYAN, BG);
        sprintf(line, "fb   %08X  stack %08X",
                (unsigned)FB_BASE, (unsigned)STACK_TOP);
        hs_puts(fb, 1, row++, line, CYAN, BG);
        row++;

        sprintf(line, "CD %u seeks / %u sectors",
                cd_seeks(), cd_sectors_read());
        hs_puts(fb, 1, row++, line, GREY, BG);

        sprintf(line, "TimerRef %lu", TimerRef);
        hs_puts(fb, 1, row++, line, GREY, BG);

        video_flip();
        TimerCountFrame();
        TRACE[8] = (u32)TimerRef;
    }
    return 0;
}
