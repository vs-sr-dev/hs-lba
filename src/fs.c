/*
 * fs.c — the flat disc archive, presented as files.
 *
 * The index at LBA 0 is { "HSLBA1", count } followed by count entries of
 * { name[16], lba, size, sum }; see tools/mkcd.py, which is the other half of
 * this format. Every file is a contiguous run of sectors, so seeking within one
 * is arithmetic and reading it is one streaming pass.
 *
 * Reads are shaped around what cd.c actually costs, and that cost is seeks, not
 * bytes. MAME charges nothing for a seek (`m_cur_sector = LEADIN + m_seek_lba;
 * // TODO: seek time`), which makes the emulator exactly the wrong instrument
 * for noticing the problem: a first version of this file split each request
 * into a head sector, a bulk run and a tail, and verified all 8.7 MB of the
 * archive in 12 seconds of emulated time while issuing something like nine
 * thousand seeks. On a real mech at 100-200 ms each that is twenty minutes of
 * head chatter to load the game.
 *
 * Hence the read-ahead run below. Sectors arrive in blocks of RA_SECTORS, and a
 * request that is both large and sector-aligned skips the buffer and streams
 * straight into the caller's memory.
 *
 * The driver underneath has since learned to carry on from where it stopped
 * rather than re-seek, and to keep the mech running between calls, so a
 * sequential pass over a file is now one seek however many cd_read()s it takes.
 * That does not make the shape below redundant: what it costs is one seek per
 * *discontinuity*, and this file is what keeps the discontinuities rare.
 */

#include "fs.h"
#include "cd.h"
#include "timer.h"

#include <string.h>

#define SECTOR      2048u
#define MAX_ENTRIES 64
#define MAX_OPEN    8

struct entry {
    char         name[16];
    unsigned int lba;
    unsigned int size;
    unsigned int sum;
};

struct handle {
    int          used;
    int          index;
    unsigned int pos;
};

static struct entry entries[MAX_ENTRIES];
static int          n_entries;
static int          mounted;

static struct handle handles[MAX_OPEN];

/* Read-ahead run: one seek fills the lot. 128 KB is a compromise — big enough
 * that the seek cost per byte disappears, small enough to sit in .bss without
 * crowding the boot-time working set. */
#define RA_SECTORS  64
#define RA_BYTES    (RA_SECTORS * SECTOR)

static unsigned char ra_buf[RA_BYTES];
static unsigned int  ra_lba;             /* first LBA held */
static unsigned int  ra_have;            /* sectors held */

unsigned int fs_fnv1a(const void *data, unsigned int len)
{
    const unsigned char *p = (const unsigned char *)data;
    unsigned int s = 2166136261u;

    while (len--) {
        s ^= *p++;
        s *= 16777619u;
    }
    return s;
}

/* Compare a caller's name against a stored one: basename only, case-folded.
 * The engine passes paths the way DOS did, and mkcd.py stores bare uppercase
 * names. */
static int name_matches(const char *want, const char *stored)
{
    const char *base = want;
    const char *p;
    int i;

    for (p = want; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':')
            base = p + 1;

    for (i = 0; i < 15; i++) {
        char a = base[i], b = stored[i];

        if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
        if (a != b)
            return 0;
        if (a == 0)
            return 1;
    }
    return 1;
}

/*
 * Make sure `lba` is in the read-ahead buffer, filling forward from it and
 * stopping at `limit` (one past the file's last sector) so read-ahead never
 * runs off the end of the archive.
 *
 * `need` is how many sectors the request in hand actually wants. Filling more
 * than that was the whole cost of a resource cache miss: an HQR miss reads four
 * bytes of offset table, seeks, reads four more, seeks again — and each of
 * those four-byte reads used to drag in the full 64 sectors. Measured on
 * SAMPLES.HQR before this changed: 256 sectors, half a megabyte, per five-
 * kilobyte sound effect, and 860 ms of frozen game to pay for it.
 *
 * Reading exactly what is asked for does not put the seeks back. The blanket
 * fill was written when cd_read() re-seeked on every call; since m13 the servo
 * keeps running and carries on from where it stopped, so consecutive small
 * reads cost a rotation between them and not a head movement, and the 448
 * sectors cd.c banks ahead already do the job this buffer was doing. What is
 * left for RA_SECTORS is a ceiling on a single transfer, not a policy.
 */
static int ra_fill(unsigned int lba, unsigned int limit, unsigned int need)
{
    unsigned int want;

    if (ra_have && lba >= ra_lba && lba < ra_lba + ra_have)
        return 0;

    want = limit - lba;
    if (want > need)
        want = need;
    if (want > RA_SECTORS)
        want = RA_SECTORS;
    if (want == 0)
        return -1;

    ra_have = 0;
    if (cd_read(lba, ra_buf, want) != 0)
        return -1;

    ra_lba = lba;
    ra_have = want;
    return 0;
}

int fs_mount(void)
{
    /* Aligned because the entries are read out as u32s: each one starts at
     * 16 + 28*i, which is a multiple of 4 only if the buffer itself is. */
    unsigned char idx[SECTOR] __attribute__((aligned(4)));
    const unsigned char *p;
    unsigned int count, i;

    mounted = 0;
    n_entries = 0;
    ra_have = 0;
    for (i = 0; i < MAX_OPEN; i++)
        handles[i].used = 0;

    if (cd_read(0, idx, 1) != 0)
        return -1;
    if (memcmp(idx, "HSLBA1", 6) != 0)
        return -1;

    count = (unsigned int)idx[8] | ((unsigned int)idx[9] << 8)
          | ((unsigned int)idx[10] << 16) | ((unsigned int)idx[11] << 24);

    /* One sector holds 72 entries; the archive is nowhere near that, and a
     * silent truncation would be worse than a short directory. */
    if (count > MAX_ENTRIES)
        count = MAX_ENTRIES;

    p = idx + 16;
    for (i = 0; i < count; i++) {
        memcpy(entries[i].name, p, 16);
        entries[i].name[15] = 0;
        entries[i].lba  = *(const unsigned int *)(p + 16);
        entries[i].size = *(const unsigned int *)(p + 20);
        entries[i].sum  = *(const unsigned int *)(p + 24);
        p += 28;
    }

    n_entries = (int)count;
    mounted = 1;
    return 0;
}

int fs_count(void) { return n_entries; }

const char *fs_name(int index)
{
    return (index >= 0 && index < n_entries) ? entries[index].name : "";
}

unsigned int fs_entry_size(int index)
{
    return (index >= 0 && index < n_entries) ? entries[index].size : 0;
}

unsigned int fs_entry_sum(int index)
{
    return (index >= 0 && index < n_entries) ? entries[index].sum : 0;
}

/* ---- Attribution -------------------------------------------------------- */

/*
 * The game freezes for a moment before each sound effect it has not played
 * before, and nothing so far said *which file* it was reading when it did. From
 * outside, a sample, an animation and a body all look the same: a synchronous
 * read in the main loop. So the counters are per directory entry rather than
 * per subsystem, and the units are the two that price a read on this console —
 * sectors off the disc, and 50 Hz ticks the game spent waiting for them.
 *
 * `fs_opens` is worth its own counter and not an implementation detail: an HQR
 * cache miss opens the archive twice (Size_HQR walks the offset table, then
 * HQR_Get walks it again), so opens-per-file is how that shows up in a number
 * instead of in a reading of the engine.
 *
 * `fs_worst_*` is the event form of the same data. A probe that looks once per
 * video frame cannot catch a stall that began and ended between two looks, but
 * it can notice that the record has moved and name the file that moved it.
 * `fs_worst_seq` changes on every new record so a poller can edge-detect.
 */
unsigned int fs_opens[MAX_ENTRIES];
unsigned int fs_reads[MAX_ENTRIES];
unsigned int fs_sectors[MAX_ENTRIES];
unsigned int fs_ticks[MAX_ENTRIES];

unsigned int fs_worst_ticks;
unsigned int fs_worst_index = ~0u;
unsigned int fs_worst_sectors;
unsigned int fs_worst_seq;

/*
 * Opens that failed for want of a handle. There are eight, and running out is
 * not a degradation — from that moment every file in the game fails to open,
 * and the symptom is a room drawn with pieces missing rather than an error.
 * That is far too quiet for something this total, so it gets a counter.
 */
unsigned int fs_open_fails;

int fs_open(const char *name)
{
    int i, h;

    if (!mounted || !name)
        return -1;

    for (i = 0; i < n_entries; i++) {
        if (!name_matches(name, entries[i].name))
            continue;

        for (h = 0; h < MAX_OPEN; h++) {
            if (!handles[h].used) {
                handles[h].used = 1;
                handles[h].index = i;
                handles[h].pos = 0;
                fs_opens[i]++;
                return h;
            }
        }
        fs_open_fails++;
        return -1;              /* out of handles */
    }
    return -1;
}

int fs_close(int h)
{
    if (h < 0 || h >= MAX_OPEN || !handles[h].used)
        return -1;
    handles[h].used = 0;
    return 0;
}

long fs_size(int h)
{
    if (h < 0 || h >= MAX_OPEN || !handles[h].used)
        return -1;
    return (long)entries[handles[h].index].size;
}

static int fs_read_body(int h, void *buf, unsigned int len)
{
    struct handle *fh;
    const struct entry *e;
    unsigned char *out = (unsigned char *)buf;
    unsigned int left, limit;

    if (h < 0 || h >= MAX_OPEN || !handles[h].used)
        return -1;

    fh = &handles[h];
    e = &entries[fh->index];

    if (fh->pos >= e->size)
        return 0;
    if (len > e->size - fh->pos)
        len = e->size - fh->pos;
    left = len;

    limit = e->lba + (e->size + SECTOR - 1) / SECTOR;   /* past the last sector */

    while (left != 0) {
        unsigned int lba = e->lba + fh->pos / SECTOR;
        unsigned int off = fh->pos % SECTOR;
        unsigned int want;

        /* A big aligned request streams past the buffer entirely: no seek per
         * run beyond the one cd_read already costs, and no extra copy. */
        if (off == 0 && left >= RA_BYTES && lba + RA_SECTORS <= limit) {
            if (cd_read(lba, out, RA_SECTORS) != 0)
                return -1;
            out += RA_BYTES;
            fh->pos += RA_BYTES;
            left -= RA_BYTES;
            /* The buffer no longer describes where we are. */
            ra_have = 0;
            continue;
        }

        if (ra_fill(lba, limit, (off + left + SECTOR - 1) / SECTOR) != 0)
            return -1;

        want = (ra_lba + ra_have - lba) * SECTOR - off;
        if (want > left)
            want = left;

        memcpy(out, ra_buf + (lba - ra_lba) * SECTOR + off, want);
        out += want;
        fh->pos += want;
        left -= want;
    }

    return (int)len;
}

/*
 * Everything the engine reads comes through here, so this is the one place that
 * knows both how long a read took and what it was reading. The clock is the
 * 50 Hz tick because it is the only one that keeps running while this blocks —
 * it is an interrupt — and 20 ms is fine resolution for a stall the player can
 * see. A read that costs zero ticks is a read that came out of ra_buf.
 */
int fs_read(int h, void *buf, unsigned int len)
{
    unsigned int t0, s0, dt, ds;
    int idx, r;

    if (h < 0 || h >= MAX_OPEN || !handles[h].used)
        return -1;

    idx = handles[h].index;
    t0 = timer_ticks();
    s0 = cd_sectors_read();

    r = fs_read_body(h, buf, len);

    dt = timer_ticks() - t0;
    ds = cd_sectors_read() - s0;

    fs_reads[idx]++;
    fs_ticks[idx] += dt;
    fs_sectors[idx] += ds;

    if (dt > fs_worst_ticks) {
        fs_worst_ticks   = dt;
        fs_worst_index   = (unsigned int)idx;
        fs_worst_sectors = ds;
        fs_worst_seq++;
    }

    return r;
}

long fs_lseek(int h, long off, int whence)
{
    struct handle *fh;
    long base, pos;

    if (h < 0 || h >= MAX_OPEN || !handles[h].used)
        return -1;

    fh = &handles[h];

    switch (whence) {
    case 0:  base = 0; break;                                  /* SEEK_SET */
    case 1:  base = (long)fh->pos; break;                      /* SEEK_CUR */
    case 2:  base = (long)entries[fh->index].size; break;      /* SEEK_END */
    default: return -1;
    }

    pos = base + off;
    if (pos < 0)
        return -1;
    /* Seeking past the end is legal; reading there returns 0. */
    fh->pos = (unsigned int)pos;
    return pos;
}
