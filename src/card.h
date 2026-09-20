#ifndef HS_CARD_H
#define HS_CARD_H

/*
 * The RFID save card (src/card.c).
 *
 * The API is deliberately shaped around the hardware's cost. A real Type 1 tag
 * writes ONE BYTE PER TRANSACTION and needs milliseconds of EEPROM programming
 * for each, so a full save is about a second of unbroken contact. There is no
 * card_write(buf) here that returns success, because such a function is
 * possible to write only in an emulator: it would compile, pass, and then fail
 * on a console the first time somebody lifted the card early.
 */

#define CARD_BYTES      120u    /* the tag's whole memory                     */
#define CARD_USER_BASE  8u      /* bytes 0-7 are UID and reserved (read-only) */
#define CARD_USER_SIZE  96u     /* 0x08..0x67; 0x68+ is reserved and lock/OTP */

/* Calibrate the delay loop. Call once, after InitTimer(). */
void card_init(void);

/* Loop iterations per microsecond, as measured against the 50 Hz tick. Only
 * of interest to the m19 milestone: if this is wildly wrong then every pulse
 * width is wrong by the same factor, and that is worth seeing as a number
 * rather than inferring from a card that will not answer. */
unsigned int card_calibration(void);

/* One REQA. 1 if a card answered. Costs about a millisecond, so it is cheap
 * enough to poll from a menu — but not cheap enough to poll every frame. */
int card_present(void);

/*
 * The 96 user bytes. One RALL if the tag will stream it (~380 ms), otherwise
 * 96 single READs (~2 s) — see card_rall_fallbacks. 1 on success.
 */
int card_read_user(unsigned char *buf);

/* The UID of the card last identified, four bytes. Valid after any successful
 * transaction. */
const unsigned char *card_uid_bytes(void);

/*
 * Writing, as three phases, because the caller has to be able to show the
 * player that something is happening for a second.
 *
 *   card_write_begin(want)  reads the card, works out which of the 96 user
 *                           bytes differ, and clears the magic so a torn write
 *                           can never be mistaken for a valid save.
 *                           Returns the number of steps to come, or -1.
 *   card_write_step()       writes one byte. >0 steps left, 0 when the payload
 *                           is done, -1 on failure.
 *   card_write_finish()     re-reads the card, checks it against `want`, and
 *                           only then writes the magic back. 1 if the save is
 *                           on the card and verified.
 *
 * `want` must point at CARD_USER_SIZE bytes and stay valid until finish.
 */
int card_write_begin(const unsigned char *want);
int card_write_step(void);
int card_write_finish(void);

/* Diagnostics: transactions issued, bytes actually programmed, parity/verify
 * failures, and how often the whole-card read had to be done the slow way. */
extern unsigned int card_transactions;
extern unsigned int card_bytes_written;
extern unsigned int card_errors;
extern unsigned int card_rall_fallbacks;

#endif
