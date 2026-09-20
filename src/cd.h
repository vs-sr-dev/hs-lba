/* cd.h — SPG290 CD servo driver for HS-LBA. */
#ifndef HS_CD_H
#define HS_CD_H

#define CD_SECTOR_USER 2048     /* MODE1 user bytes per sector */

/* Bring the servo up: buffer, sector size, speed, and the vector-60 handler
 * that bounds the prefetch. Call after irq_init(). Returns the disc ID reported
 * by the servo DSP (>= 97 means CD-ROM, < 10 means no disc). */
int  cd_init(void);

/* Read `count` consecutive MODE1 sectors starting at `lba` into `dst`
 * (count * 2048 bytes). Returns 0 on success, -1 on timeout. */
int  cd_read(unsigned int lba, void *dst, unsigned int count);

/* Non-zero while cd_read() is streaming. The ring absorbs a handler that runs
 * long (see cd.c), so this is no longer a "stand aside" order — it is still the
 * honest way to ask whether the mech is busy. */
extern volatile int cd_streaming;

/* Diagnostics for bring-up. */
unsigned int cd_dsp_version(void);
unsigned int cd_sectors_read(void);

/* Sectors the servo overwrote before the CPU copied them out. Must stay zero:
 * a lost sector surfaces far downstream as a corrupt HQR header and then as a
 * wild pointer, so it needs to be visible where it happens. */
unsigned int cd_overruns(void);

/* Head movements: requests that could not be served by carrying on from where
 * the servo already was. MAME charges nothing for one and a real mech charges
 * 100-200 ms, which makes this the only way to see the cost of a read pattern
 * from inside the emulator. Restarting a parked stream is not counted here —
 * the head is already there — and shows up in cd_resume_count instead. */
unsigned int cd_seeks(void);

/* Sectors the reader had to wait for because the bank was empty. This is the
 * measure of whether the prefetch is doing anything: a load the servo stayed
 * ahead of reports zero. */
unsigned int cd_stalls(void);

/* Sectors currently banked ahead of the reader. */
unsigned int cd_banked(void);

#endif
