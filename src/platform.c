/*
 * platform.c — the DOS-era driver layers the engine expects to find, answered
 * the way a DOS machine with none of that hardware installed would answer.
 *
 * LBA1 loads its sound and music support as Watcom DLLs, talks to CD audio
 * through a real-mode driver, and drives MIDI through AIL32. None of that
 * exists here. Every entry point below reports "no hardware, no error", with
 * the *_Driver_Enable flags left false so the engine takes the paths it takes
 * on a machine without a sound card — which are exercised paths, not error
 * paths.
 *
 * Two of these have real consequences worth stating:
 *   DriveCDR stays -1, matching the DOS driver when it finds no CD. MESSAGE.C
 *   guards SpeakFromCD() with `if (DriveCDR < 0)`; at 0 it would probe drive
 *   "A:" for every found-object voice line.
 *
 *   Midi_Driver_Enable stays false, which is what routes ADELINE.C's timer
 *   setup to InitTimer() — our 50 Hz interrupt in src/timer.c — rather than to
 *   InitMidiTimer(), where the XMI driver would have owned the clock.
 *
 * Audio and CD streaming get real implementations later (phase F); this is the
 * shape that lets the engine link and run without them.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reent.h>

#include "save.h"

typedef unsigned char  UBYTE;
typedef signed char    BYTE;
typedef unsigned short UWORD;
typedef signed short   WORD;
typedef unsigned long  ULONG;
typedef signed long    LONG;

/* An empty identifier list: the engine walks these with
 * `while (**ptridentifier)`, which stops immediately on "". */
static char *EmptyIdentList[] = { "" };
static LONG  DummyVars[8];

/* ====================================================================== */
/* Watcom DLL loader (LIB_SYS/DLL.H)                                      */
/* ====================================================================== */

ULONG DLL_size(void *source, ULONG flags)
{
    (void)source; (void)flags;
    return 0;
}

void *DLL_load(void *source, ULONG flags, void *dll)
{
    (void)source; (void)flags; (void)dll;
    return 0;
}

LONG FILE_error(void) { return 0; }

LONG FILE_size(BYTE *filename)
{
    (void)filename;
    return 0;
}

void *FILE_read(BYTE *filename, void *dest)
{
    (void)filename; (void)dest;
    return 0;
}

LONG FILE_write(BYTE *filename, void *buf, ULONG len)
{
    (void)filename; (void)buf; (void)len;
    return 0;
}

LONG FILE_append(BYTE *filename, void *buf, ULONG len)
{
    (void)filename; (void)buf; (void)len;
    return 0;
}

/* ====================================================================== */
/* CD audio (PLATFORM/DOS/LIB_CD/CDROM.ASM)                               */
/* ====================================================================== */

WORD DriveCDR = -1;
LONG FileCD_Start = 0;
LONG FileCD_Sect = 0;
LONG FileCD_Size = 0;

LONG InitCDR(char *nameid)
{
    (void)nameid;
    return 0;
}

void ClearCDR(void) { }

LONG GetFileCDR(char *name)
{
    (void)name;
    return 0;
}

LONG ReadLongCDR(LONG start, LONG nbsect, void *buffer)
{
    (void)start; (void)nbsect; (void)buffer;
    return 0;
}

/* ====================================================================== */
/* AIL32 MIDI                                                             */
/* ====================================================================== */

WORD Midi_Driver_Enable = 0;    /* false: ADELINE.C then calls InitTimer() */
LONG MaxVolume = 100;

LONG InitMidiDLL(UBYTE *driverpathname)
{
    (void)driverpathname;
    return 0;
}

void AskMidiVars(char ***listidentifier, LONG **ptrvars)
{
    *listidentifier = EmptyIdentList;
    *ptrvars = DummyVars;
}

LONG InitMidi(void)  { return 1; }
void ClearMidi(void) { }
void PlayMidi(UBYTE *ail_buffer)      { (void)ail_buffer; }
void StopMidi(void)                   { }
LONG IsMidiPlaying(void)              { return 0; }
void FadeMidiDown(WORD nbsec)         { (void)nbsec; }
void FadeMidiUp(WORD nbsec)           { (void)nbsec; }
void WaitFadeMidi(void)               { }
void VolumeMidi(WORD volume)          { (void)volume; }
void DoLoopMidi(void)                 { }
void InitPathMidiSampleFile(UBYTE *path) { (void)path; }

/* With no MIDI driver the engine never reaches this, but ADELINE.C still
 * references it. Point it at the same 50 Hz tick. */
void InitTimer(void);
void InitMidiTimer(void)
{
    InitTimer();
}

/* CD audio transport. LBA's soundtrack is Red Book on the retail disc; the
 * servo does have a CDDA mode (control0 bit 15) and this becomes real in phase
 * F. Reporting a zero-length track keeps AMBIANCE.C from waiting on music that
 * will never finish. */
void StopCDR(void) { }

LONG GetLengthTrackCDR(WORD track)
{
    (void)track;
    return 0;
}

void PlayTrackCDR(WORD track)
{
    (void)track;
}

/* ====================================================================== */
/* SYS_FILESYSTEM / SYS_TIME (PLATFORM/DOS/LIB_SYS)                       */
/* ====================================================================== */

/*
 * File enumeration. The disc archive is fixed at build time and needs none of
 * this; what does need it is the save-game list, which the engine builds by
 * globbing "*.LBA" and opening whatever it finds. So the two slots in
 * src/save.c are enumerated here and nothing else is.
 *
 * The cursor lives in `platform_handle`, which is what the field is for — the
 * engine copies SYS_FileInfo around and calls FindNext on the copy.
 */
typedef struct {
    char name[260];
    unsigned long size;
    unsigned long wr_date;
    unsigned long wr_time;
    unsigned char attrib;
    void *platform_handle;
} SYS_FileInfo;

/*
 * The pattern of the search in progress; "" means "every save". It is a single
 * static because the engine never nests one enumeration inside another — it
 * runs a FindFirst/FindNext loop to completion, opening and closing files
 * inside it, and then starts the next search from scratch.
 */
static char find_pattern[32];

/* Basename, case-folded. Only two shapes ever reach here: "*.LBA" and the name
 * of one save. */
static int name_eq(const char *a, const char *b)
{
    for (;;) {
        char ca = *a++, cb = *b++;

        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 'a' + 'A');
        if (ca != cb)
            return 0;
        if (ca == 0)
            return 1;
    }
}

static int find_step(SYS_FileInfo *info)
{
    int index = (int)(long)info->platform_handle;

    for (;;) {
        unsigned long size = 0;
        const char *name = save_enumerate(&index, &size);

        info->platform_handle = (void *)(long)index;

        if (!name) {
            info->name[0] = 0;
            return 1;
        }

        if (find_pattern[0] == 0 || name_eq(find_pattern, name)) {
            strcpy(info->name, name);
            info->size = size;
            info->attrib = 0;           /* SYS_FA_NORMAL */
            return 0;
        }
    }
}

int SYS_FindFirst(const char *pattern, unsigned attr, SYS_FileInfo *info)
{
    const char *base = pattern, *p;

    (void)attr;

    if (!info)
        return 1;

    memset(info, 0, sizeof(*info));

    for (p = pattern; p && *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':')
            base = p + 1;

    if (!base || !*base)
        return 1;

    if (base[0] == '*')
        find_pattern[0] = 0;
    else {
        strncpy(find_pattern, base, sizeof(find_pattern) - 1);
        find_pattern[sizeof(find_pattern) - 1] = 0;
    }

    return find_step(info);
}

int SYS_FindNext(SYS_FileInfo *info)
{
    if (!info)
        return 1;
    return find_step(info);
}

void SYS_FindClose(SYS_FileInfo *info)
{
    (void)info;
}

/* ====================================================================== */
/* Saved games, for the menu (engine/game/GAMEMENU.C)                     */
/* ====================================================================== */

/*
 * The engine's own save UI assumes a filesystem it can create files in. On this
 * console there is one card and one RAM slot, so these four are what GAMEMENU.C
 * calls instead of inventing filenames.
 */

int PORT_CardPresent(void)     { return save_card_present(); }
int PORT_CardSync(void)        { return save_card_sync(); }
int PORT_SaveToCard(void)      { return save_to_card(); }
int PORT_UnsavedProgress(void) { return save_unsaved_progress(); }

unsigned SYS_GetDrive(void) { return 3; }        /* "C:" */

void SYS_SetDrive(unsigned drive, unsigned *total_drives)
{
    (void)drive;
    if (total_drives)
        *total_drives = 1;
}

/* Only ever compared against the size of a save file. */
unsigned long SYS_GetDiskFreeSpace(void) { return 1024UL * 1024UL; }

/* A packed timestamp used to order save games. There is no clock on this
 * console, so hand back a counter: strictly increasing, which is the only
 * property the engine relies on. */
unsigned long SYS_ComputeTime(void)
{
    static unsigned long stamp = 0;
    return ++stamp;
}

/* ====================================================================== */
/* Watcom/DOS libc gaps                                                   */
/* ====================================================================== */

/* Watcom's DosMalloc returned a real-mode segment alongside the pointer. The
 * engine only uses the pointer. */
void *DosMalloc(LONG size, ULONG *handle)
{
    void *p = malloc((size_t)size);
    if (handle)
        *handle = 0;
    return p;
}

/* FILES_A.C Touch(): update a file's timestamp. Read-only media. */
LONG Touch(char *filename)
{
    (void)filename;
    return 0;
}

/* There is one directory and we are in it. */
char *getcwd(char *buf, size_t size)
{
    if (buf && size > 1) {
        buf[0] = '.';
        buf[1] = 0;
        return buf;
    }
    return 0;
}

int chdir(const char *path)
{
    (void)path;
    return 0;
}

/* newlib routes remove() here; nothing on the disc can be deleted. */
int _unlink_r(struct _reent *r, const char *name)
{
    (void)r; (void)name;
    return -1;
}

/* itoa/ltoa/ultoa: Watcom extensions, base 10 and 16 in practice. */
static char *utoa_base(unsigned long v, char *buf, int radix, int negative)
{
    char tmp[36];
    int i = 0, j = 0;

    if (radix < 2 || radix > 36) {
        buf[0] = 0;
        return buf;
    }

    do {
        unsigned long d = v % (unsigned long)radix;
        tmp[i++] = (char)(d < 10 ? '0' + d : 'a' + (d - 10));
        v /= (unsigned long)radix;
    } while (v);

    if (negative)
        buf[j++] = '-';
    while (i > 0)
        buf[j++] = tmp[--i];
    buf[j] = 0;
    return buf;
}

char *itoa(int value, char *buf, int radix)
{
    if (radix == 10 && value < 0)
        return utoa_base((unsigned long)(-(long)value), buf, radix, 1);
    return utoa_base((unsigned long)(unsigned int)value, buf, radix, 0);
}

char *ltoa(long value, char *buf, int radix)
{
    if (radix == 10 && value < 0)
        return utoa_base((unsigned long)(-value), buf, radix, 1);
    return utoa_base((unsigned long)value, buf, radix, 0);
}

char *ultoa(unsigned long value, char *buf, int radix)
{
    return utoa_base(value, buf, radix, 0);
}
