#ifndef HS_AUDIO_H
#define HS_AUDIO_H

/*
 * audio.h — the SPU soft channel: DAC FIFO ping-pong plus a software mixer.
 *
 * The channel table is exposed rather than wrapped because src/platform_snd.c
 * is the only user and needs to touch every field of a voice (the engine's
 * Wave API is a thin skin over exactly this state). Anything that walks the
 * table must hold audio_lock(): the mixer runs inside the SPU interrupt.
 */

/* Eight voices. Not an arbitrary number: the DOS driver's list had 50 slots
 * but only eight at full volume fit its 16-bit accumulator, and here the
 * ceiling is the interrupt budget — a fill is 1024 frames, so every extra
 * concurrent voice adds real milliseconds to a handler that must finish
 * inside a half-buffer. LBA never asks for more than a handful. */
#define AUDIO_CHANNELS 8

typedef struct {
    int            active;
    const unsigned char *data;   /* unsigned 8-bit mono PCM (VOC body) */
    unsigned long  length;       /* in samples */

    /* Cursor is an integer index plus a separate fraction, not one 16.16
     * word. A narration is longer than 65536 samples (206681 on the intro),
     * and a 16.16 cursor wraps at index 65536 — the sample silently restarts
     * and WaveInList never reports it finished, so the dialogue never ends.
     * The DOS driver kept a byte pointer and a 16-bit FRACT for this reason. */
    unsigned long  idx;
    unsigned long  frac;         /* low 16 bits used */
    unsigned long  step;         /* 16.16 increment */
    unsigned long  freq;         /* pitched rate, for logging */

    unsigned long  plays_left;   /* Repeat; 0 on entry means 65536 */
    unsigned short handle;       /* engine handle (HQR index, 0x1234 = voice) */
    unsigned long  longhandle;   /* (seq<<16)|handle, unique, never 0 */
    unsigned long  info0;
    short          vol_left;     /* engine units: 0..128 sfx, 512 voice */
    short          vol_right;
    unsigned char  interpol;     /* linear interpolation (voices only) */
} audio_chan;

extern audio_chan audio_chan_tab[AUDIO_CHANNELS];
extern volatile int audio_paused;

/* Starts the DAC. `want` is the rate asked for in LBA.CFG; the DAC divides a
 * 27 MHz clock, so the rate actually delivered is returned and is what the
 * resampling steps must be computed against. Returns 0 if the DAC refused. */
unsigned int audio_init(unsigned int want);
void         audio_shutdown(void);
unsigned int audio_rate(void);

/* Master gain applied to the whole mix, 256 = unity (the Mixer driver's
 * Wave x Master product). */
void audio_set_gain(unsigned int gain256);

/* Mask the SPU interrupt around any access to the channel table. */
unsigned int audio_lock(void);
void         audio_unlock(unsigned int state);

/* Diagnostics, read by tools/m12_audio.lua out of a running machine. */
extern volatile unsigned int audio_fills;    /* halves mixed */
extern volatile unsigned int audio_skips;    /* halves mixed while the CD ran */
extern volatile unsigned int audio_drops;    /* WavePlay with no free voice */

#endif
