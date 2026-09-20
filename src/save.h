#ifndef HS_SAVE_H
#define HS_SAVE_H

/*
 * The two save slots (src/save.c).
 *
 * There is one save *format* — the one the engine's SaveGameWithName() writes
 * for an autosave — and two places to keep it: a buffer in RAM that survives
 * until the console is switched off, and 96 bytes on the RFID card.
 *
 * The engine reaches both of them the way it reaches everything else, through
 * fopen: AUTOSAVE.LBA is the RAM slot and CARD.LBA is the card. Writing the
 * card, though, is NOT a file operation — see save_to_card().
 */

#define SAVE_MAX_BYTES  1024u   /* an autosave stream is 468; this is slack */

/* Prepare the slots and read the card once. Call after card_init(). */
void save_init(void);

/* ---- the virtual files -------------------------------------------------- */
/* Names are matched on the basename, case-folded, the way src/fs.c does. */

int  save_open(const char *name, int for_write);
int  save_read(int h, void *buf, unsigned int len);
int  save_write(int h, const void *buf, unsigned int len);
long save_lseek(int h, long off, int whence);
long save_size(int h);
int  save_close(int h);

/* Enumeration, for SYS_FindFirst/SYS_FindNext. `index` counts from 0; returns
 * the name of the next existing slot at or after it, or 0 when there are none
 * left, and advances `index` past what it returned. */
const char *save_enumerate(int *index, unsigned long *size);

/* ---- the card ----------------------------------------------------------- */

/* One REQA. Cheap enough for a menu to ask every few frames. */
int save_card_present(void);

/* Re-read the card into the CARD.LBA slot. ~370 ms, so call it when the player
 * opens a load list, not when a menu redraws. 1 if a valid save was found. */
int save_card_sync(void);

/*
 * Pack the RAM slot into 96 bytes and program the card. Synchronous, and it
 * takes between a quarter of a second and two seconds depending on how much
 * changed, all of it with the card resting on the reader.
 *
 * Returns 1 on a verified save. 0 means nothing trustworthy was written: the
 * magic byte is cleared first, so a failure leaves the card reading as empty
 * rather than as a corrupt save.
 */
int save_to_card(void);

/* Is there anything in each slot? */
int save_ram_valid(void);
int save_card_valid(void);

/*
 * Progress the player would lose by switching off now: the RAM slot has been
 * written since the last successful save_to_card(). This is what makes the
 * session-only contract visible instead of leaving it to be discovered with
 * the console already off.
 */
int save_unsaved_progress(void);

/* Diagnostics for the MAME harness. */
extern unsigned int save_ram_writes;
extern unsigned int save_card_saves;
extern unsigned int save_card_failures;
extern unsigned int save_flag_exceptions;   /* game flags that needed a full
                                             * byte on the card, high-water */
extern unsigned int save_pack_overflows;    /* saves refused for want of room */

#endif
