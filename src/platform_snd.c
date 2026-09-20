/*
 * platform_snd.c — the Wave and Mixer drivers, on top of src/audio.c.
 *
 * This replaces the DOS DLL pair W_SB16.DLL + M_SB16.DLL. The engine loads
 * them through ADELINE.C: any WaveDriver name other than "NoWave" makes it
 * call WaveInitDLL, then WaveAskVars to learn which LBA.CFG keys the driver
 * wants (the SB16 one asked for WaveBase and WaveRate; we need only WaveRate),
 * then InitWave. A missing key is exit(1), so the key list and LBA.CFG have to
 * agree.
 *
 * The sample format and every quirk below come from the original driver
 * (LIB386/PLATFORM/DOS/LIB_SAMP/WAVE.C + WAVE_A.ASM):
 *
 *  - Buffer is a Creative VOC file: u16 header size at +0x14 (26 in all LBA
 *    data), then a block of type 1: u24 block size, u8 rate byte, u8 pack
 *    (0 = raw), then unsigned 8-bit mono PCM. rate = 1000000/(256 - sr), and
 *    the sample length is blocksize - 2 because the block size counts the two
 *    bytes of rate and pack. Real rates in this data run 4098..17544 Hz.
 *
 *  - Byte 0 of the buffer is not part of the VOC as far as the game is
 *    concerned. SAMPLES.HQR entries have 'C' there (the start of "Creative");
 *    VOX voice entries have 0 or 1, which MESSAGE.C reads as FlagNextVoc to
 *    know whether another chunk of the line follows. The DOS driver reused the
 *    same byte as an interpolation flag — byte0+1 < 10 turns linear
 *    interpolation on — so voices are filtered and effects are not, and that
 *    falls out of the encoding rather than being decided anywhere.
 *
 *  - Pitchbend: 4096 is 1.0.
 *  - Repeat is a play count, and 0 means 65536 (the DOS counter underflowed).
 *  - WaveStopOne(handle) stops every voice carrying that handle.
 *  - WaveMove exists because HQR_GetSample compacts the sample heap while
 *    samples are playing; the pointers have to move with it.
 */

#include "audio.h"

typedef unsigned char  UBYTE;
typedef unsigned short UWORD;
typedef signed short   WORD;
typedef unsigned long  ULONG;
typedef signed long    LONG;

#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif

/* ---- VOC ---------------------------------------------------------------- */

typedef struct {
    const UBYTE *data;
    ULONG length;        /* samples */
    ULONG rate;          /* Hz, before pitch bend */
    UBYTE interpol;
} T_VOC;

static int ParseVoc(const UBYTE *buf, T_VOC *out)
{
    UWORD hdrsize;
    const UBYTE *body;
    ULONG blksize;

    if (!buf)
        return 0;

    hdrsize = (UWORD)(buf[0x14] | (buf[0x15] << 8));
    body = buf + hdrsize;

    if (body[0] != 1)                /* block type 1 = sound data */
        return 0;
    if (body[5] != 0)                /* pack method must be raw 8-bit */
        return 0;

    blksize = (ULONG)body[1] | ((ULONG)body[2] << 8) | ((ULONG)body[3] << 16);
    if (blksize < 2)
        return 0;

    out->length = blksize - 2;
    out->rate = 1000000UL / (256UL - (ULONG)body[4]);
    out->data = body + 6;
    out->interpol = ((UBYTE)(buf[0] + 1) < 10) ? (UBYTE)(buf[0] + 1) : 0;

    return 1;
}

/* DOS: (rate * (pb << 4)) >> 16, with carry rounding. */
static ULONG PitchedRate(ULONG rate, UWORD pitchbend)
{
    return (ULONG)(((unsigned long long)rate * ((ULONG)pitchbend << 4)
                    + 0x8000u) >> 16);
}

/* ---- LIB_SAMP / LIB_WAVE.H --------------------------------------------- */

char Wave_Driver[128];
char Wave_Driver_Name[128] = "SPG290 SPU soft channel (SB16 semantics)";
LONG Wave_Driver_Enable = 0;

/* The key list the engine will look up in LBA.CFG, empty-string terminated. */
static LONG  CfgWaveRate = 22050;
static char *WaveIdentList[] = { "WaveRate", "" };

static ULONG snd_seq;

/* Read by tools/m12_audio.lua: what the engine last asked for, so a run can
 * distinguish "the game never played anything" from "it played and we lost
 * it". */
volatile unsigned int wave_plays;
volatile unsigned int wave_last_handle;
volatile unsigned int wave_last_freq;
volatile unsigned int wave_last_len;
volatile unsigned int wave_last_vol;

/*
 * Dialogue told apart from everything else.
 *
 * MESSAGE.C plays every spoken line through the one fixed handle SPEAK_SAMPLE,
 * so the handle is the only thing that distinguishes a voice from a footstep
 * in this driver — and the two arrive by completely different routes (a voice
 * comes off the disc through an LZSS decompress into BufSpeak; a sound effect
 * comes out of the sample heap). Without separate counters a run where the
 * voices never load is indistinguishable from one where they load and play,
 * because the effects keep `wave_plays` climbing either way.
 *
 * `wave_voice_fail` is the reason WavePlay last refused a voice, which is the
 * question that matters when nothing is audible: 1 no output rate, 2 the
 * buffer did not parse as a VOC, 3 zero rate or length, 4 no free channel.
 */
#define WAVE_HANDLE_SPEAK 0x1234u   /* SPEAK_SAMPLE, engine/game/MESSAGE.C */

volatile unsigned int wave_voice_plays;
volatile unsigned int wave_voice_fail;
volatile unsigned int wave_voice_rate;
volatile unsigned int wave_voice_len;

LONG WaveInitDLL(char *dlldriver)
{
    (void)dlldriver;
    Wave_Driver_Enable = TRUE;
    return TRUE;
}

void WaveAskVars(char ***listidentifier, LONG **ptrvars)
{
    *listidentifier = WaveIdentList;
    *ptrvars = &CfgWaveRate;
}

ULONG InitWave(void)
{
    unsigned int rate;

    if (CfgWaveRate < 8000 || CfgWaveRate > 44100)
        CfgWaveRate = 22050;

    rate = audio_init((unsigned int)CfgWaveRate);

    /* A DAC that refused to start is not a reason to stop: WavePlay then
     * turns every sample down and WaveInList reports nothing playing, which
     * is the same path a PC without a sound card took — and crucially it
     * never leaves a dialogue waiting for a voice that will not finish. */
    if (!rate)
        Wave_Driver_Enable = FALSE;

    return TRUE;
}

void ClearWave(void)
{
    audio_shutdown();
}

static audio_chan *FindFree(void)
{
    int n;
    for (n = 0; n < AUDIO_CHANNELS; n++)
        if (!audio_chan_tab[n].active)
            return &audio_chan_tab[n];
    return 0;
}

ULONG WavePlay(UWORD Handle, UWORD Pitchbend, UWORD Repeat, UBYTE Follow,
               UWORD VolLeft, UWORD VolRight, void *Buffer)
{
    T_VOC voc;
    audio_chan *ch;
    ULONG pitched, lh, state;
    int voice = (Handle == WAVE_HANDLE_SPEAK);

    (void)Follow;    /* the DOS driver chained samples; the game passes 0 */

    if (!audio_rate()) {
        if (voice) wave_voice_fail = 1;
        return 0;
    }

    if (!ParseVoc((const UBYTE *)Buffer, &voc)) {
        if (voice) wave_voice_fail = 2;
        return 0;
    }

    pitched = PitchedRate(voc.rate, Pitchbend);
    if (!pitched || !voc.length) {
        if (voice) wave_voice_fail = 3;
        return 0;
    }

    state = audio_lock();

    ch = FindFree();
    if (!ch) {
        audio_unlock(state);
        audio_drops++;
        if (voice) wave_voice_fail = 4;
        return 0;
    }

    if (++snd_seq == 0)
        snd_seq = 1;
    lh = (snd_seq << 16) | Handle;

    ch->data       = voc.data;
    ch->length     = voc.length;
    ch->idx        = 0;
    ch->frac       = 0;
    ch->freq       = pitched;
    ch->step       = (ULONG)(((unsigned long long)pitched << 16) / audio_rate());
    ch->plays_left = Repeat ? Repeat : 0x10000u;   /* 0 = 65536, per DOS */
    ch->handle     = Handle;
    ch->longhandle = lh;
    ch->info0      = (ULONG)-1;
    ch->vol_left   = (WORD)VolLeft;
    ch->vol_right  = (WORD)VolRight;
    ch->interpol   = voc.interpol;
    ch->active     = 1;

    wave_plays++;
    wave_last_handle = Handle;
    wave_last_freq   = (unsigned int)pitched;
    wave_last_len    = (unsigned int)voc.length;
    wave_last_vol    = ((unsigned int)VolLeft << 16) | VolRight;

    if (voice) {
        wave_voice_plays++;
        wave_voice_rate = (unsigned int)voc.rate;
        wave_voice_len  = (unsigned int)voc.length;
    }

    audio_unlock(state);

    return lh;
}

void WaveGiveInfo0(ULONG LongHandle, ULONG Info0)
{
    ULONG state = audio_lock();
    int n;
    for (n = 0; n < AUDIO_CHANNELS; n++)
        if (audio_chan_tab[n].active && audio_chan_tab[n].longhandle == LongHandle)
            audio_chan_tab[n].info0 = Info0;
    audio_unlock(state);
}

void WaveStop(void)
{
    ULONG state = audio_lock();
    int n;
    for (n = 0; n < AUDIO_CHANNELS; n++)
        audio_chan_tab[n].active = 0;
    audio_paused = 0;
    audio_unlock(state);
}

void WaveStopOne(UWORD Handle)
{
    ULONG state = audio_lock();
    int n;
    for (n = 0; n < AUDIO_CHANNELS; n++)
        if (audio_chan_tab[n].active && audio_chan_tab[n].handle == Handle)
            audio_chan_tab[n].active = 0;
    audio_unlock(state);
}

void WaveStopOneLong(ULONG LongHandle)
{
    ULONG state = audio_lock();
    int n;
    for (n = 0; n < AUDIO_CHANNELS; n++)
        if (audio_chan_tab[n].active && audio_chan_tab[n].longhandle == LongHandle)
            audio_chan_tab[n].active = 0;
    audio_unlock(state);
}

/* MESSAGE.C's TestSpk() drives the whole dialogue system off this: it is how
 * the engine learns that a line has finished and the next chunk can be loaded.
 * An answer of "nothing is playing" that is not true ends speech early; one
 * that is stuck at "still playing" hangs Dial() forever. */
int WaveInList(UWORD handle)
{
    ULONG state = audio_lock();
    int n, found = 0;

    for (n = 0; n < AUDIO_CHANNELS; n++)
        if (audio_chan_tab[n].active && audio_chan_tab[n].handle == handle) {
            found = 1;
            break;
        }

    audio_unlock(state);
    return found;
}

/* Snapshot entries are {LongHandle, Info0} pairs — the DOS SNAP_SIZE of 8. */
static ULONG snap[AUDIO_CHANNELS][2];

int WaveGetSnap(void **Buffer)
{
    ULONG state = audio_lock();
    int n, count = 0;

    for (n = 0; n < AUDIO_CHANNELS; n++)
        if (audio_chan_tab[n].active) {
            snap[count][0] = audio_chan_tab[n].longhandle;
            snap[count][1] = audio_chan_tab[n].info0;
            count++;
        }

    audio_unlock(state);
    *Buffer = snap;
    return count;
}

/* The engine spins on `while (!WavePause());`. */
int WavePause(void)
{
    audio_paused = 1;
    return TRUE;
}

void WaveContinue(void)
{
    audio_paused = 0;
}

static audio_chan backup[AUDIO_CHANNELS];
static int        backup_paused;

void WaveSaveState(void)
{
    ULONG state = audio_lock();
    int n;
    for (n = 0; n < AUDIO_CHANNELS; n++) {
        backup[n] = audio_chan_tab[n];
        audio_chan_tab[n].active = 0;
    }
    backup_paused = audio_paused;
    audio_paused = 0;
    audio_unlock(state);
}

void WaveRestoreState(void)
{
    ULONG state = audio_lock();
    int n;
    for (n = 0; n < AUDIO_CHANNELS; n++)
        audio_chan_tab[n] = backup[n];
    audio_paused = backup_paused;
    audio_unlock(state);
}

void WaveChangeVolume(ULONG longhandle, ULONG VolGauche, ULONG VolDroit)
{
    ULONG state = audio_lock();
    int n;
    for (n = 0; n < AUDIO_CHANNELS; n++)
        if (audio_chan_tab[n].active && audio_chan_tab[n].longhandle == longhandle) {
            audio_chan_tab[n].vol_left  = (WORD)VolGauche;
            audio_chan_tab[n].vol_right = (WORD)VolDroit;
        }
    audio_unlock(state);
}

/*
 * HQR_GetSample compacts the sample heap underneath samples that are playing,
 * so the bytes move and the voices reading them have to move with them.
 *
 * The DOS version rebased anything starting at or above SrcAddr, which was
 * safe there only because BufSpeak lived in DOS low memory, always below the
 * sample heap. Here BufSpeak is an ordinary malloc and can sit above it, so
 * rebase only what is genuinely inside the moved range — otherwise a voice
 * gets silently shifted by the size of an unrelated compaction.
 */
void WaveMove(void *DestAddr, void *SrcAddr, ULONG Size)
{
    const UBYTE *src = (const UBYTE *)SrcAddr;
    long delta = (long)((UBYTE *)DestAddr - (UBYTE *)SrcAddr);
    ULONG state;
    int n;

    if (!DestAddr || !SrcAddr || !Size)
        return;

    state = audio_lock();

    {
        UBYTE *d = (UBYTE *)DestAddr;
        const UBYTE *s = src;
        ULONG i;
        if (d < s)
            for (i = 0; i < Size; i++) d[i] = s[i];
        else
            for (i = Size; i-- > 0; ) d[i] = s[i];
    }

    for (n = 0; n < AUDIO_CHANNELS; n++)
        if (audio_chan_tab[n].active &&
            audio_chan_tab[n].data >= src &&
            audio_chan_tab[n].data < src + Size)
            audio_chan_tab[n].data += delta;

    audio_unlock(state);
}

void *WaveGetAddr(void)
{
    ULONG state = audio_lock();
    void *addr = 0;
    int n;

    for (n = 0; n < AUDIO_CHANNELS; n++)
        if (audio_chan_tab[n].active) {
            addr = (void *)(audio_chan_tab[n].data + audio_chan_tab[n].idx);
            break;
        }

    audio_unlock(state);
    return addr;
}

/* ---- LIB_MIX ------------------------------------------------------------ */

/*
 * LBA.CFG keeps "MixerDriver: NoMixer" — MixerInitDLL is real engine code that
 * wants a Watcom DLL image — but the engine still calls these directly from
 * PERSO.C's ReadVolumeSettings and from the volume menu. So the SB16 mixer
 * chip is emulated in software: five stored levels, of which Wave and Master
 * actually reach the mix.
 */

LONG  Mixer_Driver_Enable = 0;
void *Mixer_listfcts = 0;

static LONG MixVol[5] = { 255, 255, 255, 255, 255 };  /* wave midi cd line master */

static char *MixerIdentList[] = { "" };
static LONG  MixerDummyVars[4];

static void UpdateGain(void)
{
    audio_set_gain((unsigned int)((MixVol[0] * MixVol[4] * 256) / (255 * 255)));
}

void MixerAskVars(char ***listidentifier, LONG **ptrvars)
{
    *listidentifier = MixerIdentList;
    *ptrvars = MixerDummyVars;
}

void MixerChangeVolume(LONG VolWave, LONG VolMidi, LONG VolCD,
                       LONG VolLine, LONG VolMaster)
{
    if (VolWave   != -1) MixVol[0] = VolWave   & 255;
    if (VolMidi   != -1) MixVol[1] = VolMidi   & 255;
    if (VolCD     != -1) MixVol[2] = VolCD     & 255;
    if (VolLine   != -1) MixVol[3] = VolLine   & 255;
    if (VolMaster != -1) MixVol[4] = VolMaster & 255;
    UpdateGain();
}

/* A null pointer means "do not report this one". */
static void put(LONG *p, LONG v) { if (p) *p = v; }

void MixerGetVolume(LONG *VolWave, LONG *VolMidi, LONG *VolCD,
                    LONG *VolLine, LONG *VolMaster)
{
    put(VolWave, MixVol[0]); put(VolMidi, MixVol[1]); put(VolCD, MixVol[2]);
    put(VolLine, MixVol[3]); put(VolMaster, MixVol[4]);
}

void MixerGetInfo(LONG *VolWave, LONG *VolMidi, LONG *VolCD,
                  LONG *VolLine, LONG *VolMaster)
{
    /* All five sliders exist, as on an SB16 — this is what makes the volume
     * page of the game menu operable. */
    put(VolWave, 1); put(VolMidi, 1); put(VolCD, 1);
    put(VolLine, 1); put(VolMaster, 1);
}
