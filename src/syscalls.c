/*
 * syscalls.c — the newlib bottom end, replacing the SDK's libgloss.c.
 *
 * Two things had to change and neither could be done by adding code alongside
 * the SDK's version, since these symbols can only be defined once.
 *
 * The heap. libgloss.c allocates it as `static unsigned char _heap[HEAPSIZE]`,
 * an 8 MB object in .bss. That put the heap at 0xA009C948..0xA089DE88 with the
 * framebuffers at 0xA0500000 sitting inside it, and crt0 dutifully zeroing all
 * 8 MB at boot. It had never misbehaved only because nothing had called malloc
 * yet. sbrk here hands out the real gap between the end of .bss and a fixed
 * ceiling below the framebuffers — about 14 MB — and refuses past it, so an
 * over-allocation is a NULL from malloc instead of a heap write landing on the
 * screen.
 *
 * The filesystem. libgloss.c's _open_r returns -1 unconditionally, so fopen
 * cannot work at all, and _fstat_r claims every descriptor is a character
 * device, which would make newlib treat a data file as an unseekable stream.
 * Descriptors 0-2 still go to the UART exactly as before; anything else is a
 * file in the disc archive (src/fs.c), which is what lets the engine's FILES.C
 * keep calling fopen/fread/fseek unmodified.
 *
 * Saved games are the one thing the disc cannot be: src/save.c holds them, and
 * its two slots appear here as descriptors of their own from FD_SAVE_BASE up.
 * They are checked before the archive, and they are the only descriptors on
 * this console that a write can succeed on.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "fs.h"
#include "save.h"
#include "memmap.h"

/* End of .bss, provided by the linker script. */
extern char _end[];

#define FD_FIRST_FILE 3

/*
 * Well clear of the eight archive descriptors, so that a stray fd from one
 * world can never be read as belonging to the other. There is no fd table to
 * consult here — the range IS the type.
 */
#define FD_SAVE_BASE  32

static int is_save_fd(int fd) { return fd >= FD_SAVE_BASE; }

#define UART_DATA   (*(volatile unsigned int *)0x88150000u)
#define UART_STATUS (*(volatile unsigned int *)0x88150010u)
#define UART_TX_BUSY 0x00000020u
#define UART_RX_EMPTY 0x00000010u

static unsigned int heap_brk;
static unsigned int heap_high_water;

/*
 * A console in RAM.
 *
 * The engine is full of printf() tracing its own boot, which is exactly what
 * you want when it stops without faulting — but the UART goes nowhere: MAME
 * leaves the port unmapped (the printf hook in spg29x_mame.cpp is commented
 * out), and on real hardware nothing is plugged into it either. So everything
 * written to stdout and stderr is also appended to a ring buffer at a fixed
 * address, where a Lua script can read it straight out of memory.
 *
 * A ring rather than a log: a boot that loops forever printing must not run off
 * the end of RAM. The header carries a magic word and a monotonic total so a
 * reader can tell wraps from a stalled machine.
 */
#define CON_BASE   (TRACE_BASE + 0x1000u)
#define CON_SIZE   0x4000u              /* 16 KB of text */

#define CON_MAGIC  (*(volatile unsigned int *)(CON_BASE))
#define CON_TOTAL  (*(volatile unsigned int *)(CON_BASE + 4))
#define CON_TEXT   ((volatile unsigned char *)(CON_BASE + 16))

static void con_putc(char c)
{
    if (CON_MAGIC != 0xC04501E1u) {
        CON_MAGIC = 0xC04501E1u;
        CON_TOTAL = 0;
    }
    CON_TEXT[CON_TOTAL % (CON_SIZE - 16u)] = (unsigned char)c;
    CON_TOTAL = CON_TOTAL + 1u;
}

void _putc_r(char ch)
{
    con_putc(ch);

    while (UART_STATUS & UART_TX_BUSY) { }

    if (ch == '\n' || ch == '\r') {
        UART_DATA = '\r';
        while (UART_STATUS & UART_TX_BUSY) { }
        UART_DATA = '\n';
    } else {
        UART_DATA = ch;
    }
}

int isatty(int fd)
{
    return fd >= 0 && fd < FD_FIRST_FILE;
}

int _read_r(struct _reent *r, int fd, char *buf, int nbytes)
{
    (void)r;

    if (isatty(fd)) {
        int left = nbytes;
        while (left-- > 0) {
            while (UART_STATUS & UART_RX_EMPTY) { }
            *buf = (char)UART_DATA;
            _putc_r(*buf);
            if (*buf == '\r' || *buf == '\n')
                break;
            buf++;
        }
        return nbytes - left;
    }

    if (is_save_fd(fd))
        return save_read(fd - FD_SAVE_BASE, buf, (unsigned int)nbytes);

    return fs_read(fd - FD_FIRST_FILE, buf, (unsigned int)nbytes);
}

int _write_r(struct _reent *r, int fd, char *buf, int nbytes)
{
    int n = nbytes;

    if (is_save_fd(fd))
        return save_write(fd - FD_SAVE_BASE, buf, (unsigned int)nbytes);

    if (!isatty(fd)) {
        /* The disc is read-only, and the card is not written through fopen —
         * see save.h. */
        r->_errno = EBADF;
        return -1;
    }

    while (n-- > 0)
        _putc_r(*buf++);

    return nbytes;
}

int _open_r(struct _reent *r, const char *name, int flags, int mode)
{
    int for_write = (flags & (O_WRONLY | O_RDWR)) != 0;
    int h;

    (void)mode;

    h = save_open(name, for_write);
    if (h >= 0)
        return h + FD_SAVE_BASE;

    if (for_write) {
        r->_errno = EROFS;
        return -1;
    }

    h = fs_open(name);
    if (h < 0) {
        r->_errno = ENOENT;
        return -1;
    }
    return h + FD_FIRST_FILE;
}

int _close_r(struct _reent *r, int fd)
{
    (void)r;

    if (is_save_fd(fd))
        return save_close(fd - FD_SAVE_BASE);

    if (isatty(fd))
        return 0;
    return fs_close(fd - FD_FIRST_FILE);
}

off_t _lseek_r(struct _reent *r, int fd, off_t offset, int whence)
{
    (void)r;

    if (is_save_fd(fd))
        return (off_t)save_lseek(fd - FD_SAVE_BASE, (long)offset, whence);

    if (isatty(fd))
        return (off_t)-1;
    return (off_t)fs_lseek(fd - FD_FIRST_FILE, (long)offset, whence);
}

int _fstat_r(struct _reent *r, int fd, struct stat *buf)
{
    (void)r;

    memset(buf, 0, sizeof(*buf));

    if (isatty(fd)) {
        buf->st_mode = S_IFCHR;
        return 0;
    }

    /* A real size and S_IFREG: newlib decides buffering and whether a stream
     * is seekable from this, and calling a 4 MB HQR a terminal would cost both. */
    buf->st_mode = S_IFREG;
    buf->st_blksize = 2048;

    if (is_save_fd(fd)) {
        buf->st_size = save_size(fd - FD_SAVE_BASE);
        return 0;
    }

    buf->st_size = fs_size(fd - FD_FIRST_FILE);
    return 0;
}

char *_sbrk_r(struct _reent *r, int nbytes)
{
    unsigned int prev;

    if (heap_brk == 0)
        heap_brk = ((unsigned int)_end + 3u) & ~3u;

    prev = heap_brk;

    if (nbytes > 0 && (unsigned int)nbytes > HEAP_END - heap_brk) {
        r->_errno = ENOMEM;
        return (char *)-1;
    }

    heap_brk += (unsigned int)nbytes;
    if (heap_brk > heap_high_water)
        heap_high_water = heap_brk;

    return (char *)prev;
}

/* Diagnostics for the platform layer: how much of the heap is committed and
 * how much remains before the ceiling. */
unsigned int heap_used(void)
{
    unsigned int base = ((unsigned int)_end + 3u) & ~3u;
    return (heap_brk ? heap_brk : base) - base;
}

unsigned int heap_free(void)
{
    unsigned int brk = heap_brk ? heap_brk : (((unsigned int)_end + 3u) & ~3u);
    return HEAP_END - brk;
}

unsigned int heap_base(void)
{
    return ((unsigned int)_end + 3u) & ~3u;
}

void _exit(int extcode)
{
    (void)extcode;
    for (;;) { }
}

int _getpid(void)                    { return 1; }
int _getpid_r(struct _reent *r)      { (void)r; return 1; }

int _kill(int pid, int sig)
{
    (void)pid; (void)sig;
    errno = EINVAL;
    return -1;
}

int _kill_r(struct _reent *r, int pid, int sig)
{
    (void)pid; (void)sig;
    r->_errno = EINVAL;
    return -1;
}
