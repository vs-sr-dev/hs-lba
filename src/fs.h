#ifndef HS_FS_H
#define HS_FS_H

/*
 * The disc as a filesystem.
 *
 * There is no ISO9660 parser here and there will not be one: tools/mkcd.py
 * lays the data out as a flat archive with a fixed index at LBA 0 and every
 * file stored contiguously, because on this console seeks cost far more than
 * throughput. A file is therefore nothing but a start LBA and a length, and
 * opening one is a table lookup.
 *
 * src/syscalls.c hangs newlib's open/read/lseek/close off this, so the engine's
 * FILES.C keeps using plain fopen/fread and never learns where its data is.
 */

int  fs_mount(void);                    /* read the index; 0 on success */

int  fs_open(const char *name);         /* handle, or -1 if not present */
int  fs_close(int h);
int  fs_read(int h, void *buf, unsigned int len);   /* bytes read, -1 on error */
long fs_lseek(int h, long off, int whence);
long fs_size(int h);

/* Directory, for diagnostics and for the boot-time preload. */
int          fs_count(void);
const char  *fs_name(int index);
unsigned int fs_entry_size(int index);
unsigned int fs_entry_sum(int index);   /* FNV-1a over the file, from mkcd.py */

/* FNV-1a, matching tools/mkcd.py, so a read can be checked end to end without
 * a host in the loop. */
unsigned int fs_fnv1a(const void *data, unsigned int len);

/*
 * Read attribution, indexed the same way fs_name() is. tools/m18_fetch.lua
 * reads these by name out of a running machine to answer the one question the
 * counters in cd.c cannot: a stall says the disc was busy, this says what for.
 *
 *   fs_opens    times the file was opened. Two per HQR cache miss is Size_HQR
 *               walking the offset table and HQR_Get walking it again.
 *   fs_reads    fs_read() calls
 *   fs_sectors  sectors cd.c actually delivered on this file's behalf
 *   fs_ticks    50 Hz ticks spent inside fs_read() for it — i.e. frames the
 *               game did not draw
 *
 * fs_worst_* is the longest single read on record, kept as an event: bump
 * fs_worst_seq is the edge a poller watches, and the other three describe it.
 */
extern unsigned int fs_opens[];
extern unsigned int fs_reads[];
extern unsigned int fs_sectors[];
extern unsigned int fs_ticks[];

/* Opens refused for want of one of the eight handles. Non-zero means the game
 * has stopped being able to read the disc at all — see src/fs.c. */
extern unsigned int fs_open_fails;

extern unsigned int fs_worst_ticks;
extern unsigned int fs_worst_index;
extern unsigned int fs_worst_sectors;
extern unsigned int fs_worst_seq;

#endif
