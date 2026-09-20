/*
 * audio.c — the SPU software channel: DAC FIFO ping-pong plus a mixer.
 *
 * There are two ways to make noise on this chip and only one of them is
 * attested by first-party code.
 *
 * The 24 hardware channels would be nearly free — they fetch and decode ADPCM
 * themselves — but they want their own format in their own address window (a
 * 22-bit *word* address, so the low 8 MB only), LBA's samples are 8-bit VOC
 * PCM scattered across a heap that reaches 14 MB, and MAME's model of those
 * channels was reverse-engineered from one retail game with undocumented mode
 * bits. That is three guesses stacked.
 *
 * The DAC FIFO is documented by code that shipped: PPCSDK
 * HyperScan-SPG29x-SDK .../USBLoader/MP3Drv/MP3Drv.c drives it, and every
 * register value below is copied from there rather than deduced:
 *
 *     *P_DAC_SAMPLE_CLK  = 27000000/rate - 1
 *     *P_DAC_INT_STATUS  = 0x4000 | 0x0004 | 0x0003   enable, stereo, 8 KB
 *     *P_DAC_INT_STATUS  = 0xC000 | 0x0004 | 0x0003   acknowledge, in the ISR
 *     *P_DAC_MODE_CTRL2  = 0x0608 | VolSel | 0x1003
 *
 * with a `unsigned short FIFOArray[4096]` filled 2048 words at a time,
 * alternating the write offset between 0 and 2048 — one refill per interrupt.
 * Its Left_Only path writes the same value to [i*2] and [i*2+1], which is what
 * pins the layout down: frames are interleaved L,R. Samples are stored
 * *unsigned* (`TempValue ^= 0x8000`), so silence is 0x8000 and a zeroed buffer
 * is full-scale DC, not quiet.
 *
 * Where the mixing happens. A half is 1024 frames — 46 ms — and a full LBA
 * redraw costs more than that, so the main loop cannot be trusted to refill in
 * time and the mix runs in the interrupt. That is only safe because m10 fixed
 * Sys_isr.s to save CEH/CEL and sr0; before that, a handler doing real work
 * corrupted the interrupted code's hardware loop counter.
 *
 * That still leaves it colliding with CD transfers, and the first version of
 * this file stood aside while cd_streaming was set — which during a movie, when
 * the disc streams continuously, meant replaying the same 46 ms over and over
 * at about 5 Hz. Audible, and regular enough to sound like a bug in the movie
 * player rather than in the mixer. The fix belonged in cd.c: its ring is now 64
 * frames deep, so the CPU may fall milliseconds behind and catch up. The
 * counter below stays, because "how often did sound and disc want the CPU at
 * the same moment" is still worth being able to read.
 *
 * The mixer itself is a port of the DOS driver's semantics
 * (LIB386/PLATFORM/DOS/LIB_SAMP/WAVE.C + WAVE_A.ASM), by way of the SDL shim
 * in the DS port, which is where they were first worked out and tested.
 */

#include "audio.h"
#include "irq.h"
#include "cd.h"

typedef volatile unsigned int vu32;

/* Names and addresses from sdk/.../include/SPG290_Registers.h. */
#define P_SPU_CH_ENABLE      (*(vu32 *)0x88051000u)
#define P_SPU_MAIN_VOLUME    (*(vu32 *)0x88051004u)
#define P_DAC_MODE_CTRL2     (*(vu32 *)0x88051034u)
#define P_DAC_SAMPLE_CLK     (*(vu32 *)0x88051064u)
#define P_DAC_FIFOBA_LOW     (*(vu32 *)0x88051080u)
#define P_DAC_FIFOBA_HIGH    (*(vu32 *)0x88051084u)
#define P_DAC_INT_STATUS     (*(vu32 *)0x88051088u)
#define P_DAC_BUFFER_SA      (*(vu32 *)0x88070054u)

#define IRQ_SPU              63          /* SPU FIQ, per the SoC vector list */

#define DAC_CLK_HZ           27000000u

/* Buffer size field of P_DAC_INT_STATUS. Only 3 is attested (MP3Drv asks for
 * "8Kbytes" and hands over a 4096-word array), so 3 is what we ask for. */
#define DAC_SIZE_8K          0x0003u
#define DAC_STEREO           0x0004u
#define DAC_ENABLE           0x4000u
#define DAC_IRQ              0x8000u

#define DAC_MODE             (DAC_STEREO | DAC_SIZE_8K)
#define DAC_ARM              (DAC_ENABLE | DAC_MODE)
#define DAC_ACK              (DAC_IRQ | DAC_ENABLE | DAC_MODE)

/* VolSel is bits 7:6 — 1/32, 1/8, 1/2, 1. MP3Drv picks 1/8 because it is
 * feeding decoded MP3 that already sits near full scale; our mix is scaled to
 * leave headroom, so take the whole thing. */
#define DAC_VOLSEL_FULL      (3u << 6)
#define DAC_MODE_CTRL2       (0x0608u | DAC_VOLSEL_FULL | 0x1003u)

#define FIFO_FRAMES          2048u       /* 8 KB / 2 channels / 2 bytes */
#define HALF_FRAMES          (FIFO_FRAMES / 2)
#define HALF_WORDS           (HALF_FRAMES * 2)

/* Silence is 0x8000, not 0: the FIFO holds unsigned PCM. */
#define PCM_SILENCE          0x8000u

static unsigned short fifo[FIFO_FRAMES * 2];
static int            fill_half;
static unsigned int   gain256 = 256;

/* Not static, and neither are the counters below: tools/m12_audio.lua reads
 * them by name out of a running machine. */
volatile unsigned int audio_out_rate;

/* Per-frame stereo accumulator, only needed when two or more voices overlap.
 * Kept out of the ISR's stack: the exception stack is not sized for 8 KB. */
static int accum[HALF_FRAMES * 2];

/* Whether each half already holds silence, so an idle machine stops writing. */
static int silent[2];

audio_chan   audio_chan_tab[AUDIO_CHANNELS];
volatile int audio_paused;

volatile unsigned int audio_fills;
volatile unsigned int audio_skips;
volatile unsigned int audio_drops;


unsigned int audio_lock(void)   { return irq_disable(); }
void audio_unlock(unsigned int state) { irq_restore(state); }

unsigned int audio_rate(void)   { return audio_out_rate; }

void audio_set_gain(unsigned int g) { gain256 = g; }


/* ------------------------------------------------------------------------ */
/*  The mixer                                                                */
/* ------------------------------------------------------------------------ */

/* Clip a mixed sample and store it as the unsigned PCM the FIFO wants. */
static void put_frame(unsigned short *out, int l, int r)
{
    if (l >  32767) l =  32767;
    if (l < -32768) l = -32768;
    if (r >  32767) r =  32767;
    if (r < -32768) r = -32768;
    out[0] = (unsigned short)(l + 32768);
    out[1] = (unsigned short)(r + 32768);
}


/*
 * One voice, straight into the FIFO.
 *
 * Worth having as its own loop rather than a case of the general one. The
 * general path costs five passes over the half — clear the accumulator, mix,
 * then scale and clip and convert — and this does it in one. It matters
 * because the case is not rare: a movie plays exactly one sample at a time
 * from FLASAMP, continuously, and the frames it is stuttering are paced on the
 * 50 Hz tick, so a handler that overruns by a little drops a whole 20 ms.
 */
static void mix_one(unsigned short *out, audio_chan *ch)
{
    int vl = (ch->vol_left  >> 2) * (int)gain256 >> 8;
    int vr = (ch->vol_right >> 2) * (int)gain256 >> 8;
    unsigned int i;

    for (i = 0; i < HALF_FRAMES; i++) {
        int s;

        if (ch->idx >= ch->length) {
            if (--ch->plays_left == 0) {
                ch->active = 0;
                break;
            }
            ch->idx = 0;
            ch->frac = 0;
        }

        s = (int)ch->data[ch->idx] - 128;

        if (ch->interpol && ch->idx + 1 < ch->length) {
            int s2 = (int)ch->data[ch->idx + 1] - 128;
            s += ((s2 - s) * (int)((ch->frac >> 8) & 0xFF)) >> 8;
        }

        put_frame(out + i * 2, s * vl, s * vr);

        ch->frac += ch->step;
        ch->idx  += ch->frac >> 16;
        ch->frac &= 0xFFFF;
    }

    for (; i < HALF_FRAMES; i++) {          /* sample ended inside the half */
        out[i * 2 + 0] = PCM_SILENCE;
        out[i * 2 + 1] = PCM_SILENCE;
    }
}


static void mix_half(unsigned short *out, int half)
{
    int n, i, nactive = 0;
    audio_chan *only = 0;

    if (!audio_paused) {
        for (n = 0; n < AUDIO_CHANNELS; n++)
            if (audio_chan_tab[n].active) {
                nactive++;
                only = &audio_chan_tab[n];
            }
    }

    /* Nothing playing is the normal state, and the cheapest thing to do about
     * it is nothing at all: once a half holds silence it keeps holding it. */
    if (nactive == 0) {
        if (silent[half])
            return;
        for (i = 0; i < (int)(HALF_FRAMES * 2); i++)
            out[i] = PCM_SILENCE;
        silent[half] = 1;
        return;
    }

    silent[half] = 0;

    if (nactive == 1) {
        mix_one(out, only);
        return;
    }

    for (i = 0; i < (int)(HALF_FRAMES * 2); i++)
        accum[i] = 0;

    {
        for (n = 0; n < AUDIO_CHANNELS; n++) {
            audio_chan *ch = &audio_chan_tab[n];
            int vl, vr;

            if (!ch->active)
                continue;

            /* The DOS SB16 build pre-shifted the volume by SHIFT_SAMPLE-1
             * before multiplying an 8-bit sample by it, which is what makes
             * eight voices at full volume saturate a 16-bit accumulator
             * exactly. Keep the arithmetic identical: the game's "128 max"
             * is not respected by the game itself (voices pass 512, and
             * GiveBalance reaches 724), so any rescaling here changes the
             * relative loudness of speech against effects. */
            vl = ch->vol_left >> 2;
            vr = ch->vol_right >> 2;

            for (i = 0; i < (int)HALF_FRAMES && ch->active; i++) {
                int s;

                if (ch->idx >= ch->length) {
                    if (--ch->plays_left == 0) {
                        ch->active = 0;
                        break;
                    }
                    ch->idx = 0;
                    ch->frac = 0;      /* DOS resets FRACT on repeat */
                }

                s = (int)ch->data[ch->idx] - 128;

                if (ch->interpol && ch->idx + 1 < ch->length) {
                    int s2 = (int)ch->data[ch->idx + 1] - 128;
                    s += ((s2 - s) * (int)((ch->frac >> 8) & 0xFF)) >> 8;
                }

                accum[i * 2 + 0] += s * vl;
                accum[i * 2 + 1] += s * vr;

                ch->frac += ch->step;
                ch->idx  += ch->frac >> 16;
                ch->frac &= 0xFFFF;
            }
        }
    }

    for (i = 0; i < (int)HALF_FRAMES; i++) {
        /* gain256 is 0..256 and the accumulator cannot exceed ~2^18, so this
         * stays inside 32 bits and needs no long long — which on this
         * compiler would be a library call per sample. */
        put_frame(out + i * 2,
                  (accum[i * 2 + 0] * (int)gain256) >> 8,
                  (accum[i * 2 + 1] * (int)gain256) >> 8);
    }
}


static void audio_isr(void)
{
    /* Acknowledge first: the ack must carry the enable and mode bits, exactly
     * as MP3Drv's ISR writes them, or the write turns the channel off instead
     * of clearing the flag. */
    P_DAC_INT_STATUS = DAC_ACK;

    if (cd_streaming)
        audio_skips++;      /* not skipped any more — just counted, see cd.c */

    mix_half(&fifo[fill_half * HALF_WORDS], fill_half);
    audio_fills++;
    fill_half ^= 1;
}


/* ------------------------------------------------------------------------ */
/*  Bring-up                                                                 */
/* ------------------------------------------------------------------------ */

unsigned int audio_init(unsigned int want)
{
    unsigned int div, i;

    if (want < 8000u || want > 44100u)
        want = 22050u;

    /* 27 MHz / (div + 1). The rate that comes back out is not the rate asked
     * for and the resampling steps must use the real one. */
    div = DAC_CLK_HZ / want;
    if (div == 0)
        return 0;
    div -= 1;
    audio_out_rate = DAC_CLK_HZ / (div + 1);

    for (i = 0; i < AUDIO_CHANNELS; i++)
        audio_chan_tab[i].active = 0;
    audio_paused = 0;
    audio_fills = audio_skips = audio_drops = 0;

    /* A zeroed buffer would be full-scale DC. */
    for (i = 0; i < FIFO_FRAMES * 2; i++)
        fifo[i] = PCM_SILENCE;
    silent[0] = silent[1] = 1;

    P_SPU_CH_ENABLE   = 0;              /* no hardware voices, we mix in CPU */
    P_SPU_MAIN_VOLUME = 0x7f;
    P_DAC_INT_STATUS  = 0;              /* stop before reprogramming */
    P_DAC_MODE_CTRL2  = 0;

    P_DAC_BUFFER_SA   = 0;
    P_DAC_FIFOBA_LOW  = (unsigned int)fifo;
    P_DAC_FIFOBA_HIGH = ((unsigned int)fifo) >> 16;
    P_DAC_SAMPLE_CLK  = div;

    irq_set_handler(IRQ_SPU, audio_isr);

    /* The DAC starts at the beginning of the buffer, so the first interrupt
     * arrives once half 0 has drained and asks for half 0 back. */
    fill_half = 0;

    P_DAC_INT_STATUS  = DAC_ARM;
    P_DAC_MODE_CTRL2  = DAC_MODE_CTRL2;

    return audio_out_rate;
}


void audio_shutdown(void)
{
    unsigned int i;

    P_DAC_INT_STATUS = 0;
    P_DAC_MODE_CTRL2 = 0;
    irq_set_handler(IRQ_SPU, 0);

    for (i = 0; i < AUDIO_CHANNELS; i++)
        audio_chan_tab[i].active = 0;

    audio_out_rate = 0;
}
