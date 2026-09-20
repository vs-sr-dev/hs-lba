/*
 * save.c — where a saved game lives on a console with no storage.
 *
 * TWO SLOTS, ONE FORMAT.
 *
 * The engine has exactly one serialiser, SaveGameWithName() in GAMEMENU.C, and
 * in its autosave form it emits a 468-byte stream. Both slots here hold that
 * same stream, so LoadGame() reads them without knowing the difference:
 *
 *   AUTOSAVE.LBA   a buffer in RAM. Written on every scene change and on every
 *                  death, exactly as the DOS game wrote its file. Costs nothing,
 *                  cannot fail, needs no card — and is gone when the console is
 *                  switched off.
 *   CARD.LBA       96 bytes on the RFID card, written only when the player asks
 *                  and only with the card already resting on the reader.
 *
 * WHY THE CARD NEEDS A SECOND FORMAT ANYWAY.
 *
 * 468 bytes do not fit in 96. Almost all of the 468 is three arrays that are
 * morally bitmaps and physically byte arrays:
 *
 *   ListFlagGame[255]       255 bits =  32 bytes
 *   TabHoloPos[150]         150 bits =  19 bytes
 *   ListFlagInventory[28]    28 bits =   4 bytes
 *
 * TabHoloPos is safe to squeeze: HOLOMAP.C only ever stores 0, 129 or 64 in it,
 * and the sole read of bit 64 is commented out, so one bit per position loses
 * nothing that any code looks at. ListFlagInventory is only ever set to TRUE.
 *
 * ListFlagGame is NOT safe to squeeze, and this is the part worth being honest
 * about. GERELIFE.C's LM_SET_FLAG_GAME copies a byte straight out of a life
 * script, so a flag may legitimately hold any value at all; which of the 255
 * are counters rather than booleans is a property of the game's scripts and
 * nobody here has surveyed them. Guessing they are all boolean would corrupt a
 * save quietly, months from now, in whichever scene first uses a counter.
 *
 * So the bitmap records "flag is nonzero", and any flag holding something other
 * than 0 or 1 is additionally written out in full in an exception list. Seven
 * slots fit. If a save ever needs an eighth the save is REFUSED, loudly, rather
 * than silently truncated — and save_flag_exceptions records the high-water
 * mark so playing the game is what tells us how many are really needed.
 *
 * WHAT THE CARD SAVE DOES NOT CARRY.
 *
 * The DOS manual save also wrote ListObjet, ListExtra, the zone list and the
 * per-cube flags: a few kilobytes, which no amount of packing brings near 96.
 * A card save therefore restores the same things an autosave does — the scene
 * and the player's state, not the exact arrangement of the objects in the room
 * being left. That is a consequence of 96 bytes, not a shortcut, and it is why
 * the manual save writes the autosave form deliberately rather than by accident.
 */

#include "save.h"
#include "card.h"

#include <string.h>

/* ---- the on-card layout ------------------------------------------------- */
/*
 * 96 bytes, at card offset 8. Byte 0 is the magic and is programmed LAST, so a
 * card lifted mid-save reads as empty rather than as a save that is half old
 * and half new.
 */
#define C_MAGIC        0u       /* 'L' when valid, 0 while being written  */
#define C_VERSION      1u
#define C_SUM          2u
#define C_FLAGS        3u       /* 32 bytes: 255 game flags, one bit each */
#define C_HOLO        35u       /* 19 bytes: 150 holomap positions        */
#define C_INV         54u       /*  4 bytes: 28 inventory-used flags      */
#define C_NUMCUBE     58u
#define C_CHAPITRE    59u
#define C_COMPORT     60u
#define C_LIFE        61u
#define C_GOLD        62u       /* 2 */
#define C_MAGICLEVEL  64u
#define C_MAGICPOINT  65u
#define C_CLOVERBOX   66u
#define C_CLOVER      67u
#define C_FUEL        68u
#define C_WEAPON      69u       /* 2 */
#define C_STARTX      71u       /* 2 */
#define C_STARTY      73u       /* 2 */
#define C_STARTZ      75u       /* 2 */
#define C_BETA        77u       /* 2 */
#define C_GENBODY     79u
#define C_NEXC        80u
#define C_EXC         81u       /* 7 pairs of (flag index, value) */
#define C_END         95u       /* reserved, kept zero */

#define MAX_EXC        7u
#define CARD_MAGIC  0x4Cu       /* 'L' */
#define CARD_FORMAT    1u

/* The engine's array sizes. Kept as literals rather than pulled in from
 * COMMON.H: this file is compiled with the platform's flags, not the engine's,
 * and a mismatch here has to fail at the length check below rather than by
 * quietly agreeing with a stale header. */
#define N_FLAGS      255u
#define N_HOLO       150u
#define N_INV         28u

/* The holomap value that means "arrow visible"; see HOLOMAP.C SetHoloPos(). */
#define HOLO_SET     (1u + 128u)

unsigned int save_ram_writes;
unsigned int save_card_saves;
unsigned int save_card_failures;
unsigned int save_flag_exceptions;
unsigned int save_pack_overflows;

/* ---- the slots ---------------------------------------------------------- */

struct slot {
    const char   *name;
    unsigned char data[SAVE_MAX_BYTES];
    unsigned int  len;
    int           valid;
};

static struct slot ram  = { "AUTOSAVE.LBA", { 0 }, 0, 0 };
static struct slot card = { "CARD.LBA",     { 0 }, 0, 0 };

/* Bumped by every write to the RAM slot, and copied into `saved_gen` by a
 * successful card save. Unequal means unsaved progress. */
static unsigned int ram_gen;
static unsigned int saved_gen;

#define MAX_SAVE_OPEN 2

struct save_handle {
    int          used;
    struct slot *s;
    unsigned int pos;
    int          writing;
};

static struct save_handle handles[MAX_SAVE_OPEN];

/* ---- names -------------------------------------------------------------- */

/* Basename, case-folded, the same rule src/fs.c uses — the engine builds paths
 * the way DOS did and PATH_RESSOURCE is empty in this build anyway. */
static int name_is(const char *want, const char *slotname)
{
    const char *base = want, *p;
    int i;

    if (!want)
        return 0;

    for (p = want; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':')
            base = p + 1;

    for (i = 0; i < 32; i++) {
        char a = base[i], b = slotname[i];

        if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
        if (a != b)
            return 0;
        if (a == 0)
            return 1;
    }
    return 1;
}

static struct slot *slot_for(const char *name)
{
    if (name_is(name, ram.name))
        return &ram;
    if (name_is(name, card.name))
        return &card;
    return 0;
}

/* ---- reading the engine's stream ---------------------------------------- */

/*
 * A cursor over the autosave stream. Everything is parsed in the order
 * SaveGameWithName() writes it rather than at fixed offsets, because the name
 * field is variable length; and every read is bounds-checked, because this
 * parser is also what a corrupt card save arrives through.
 */
struct cur {
    const unsigned char *p;
    unsigned int         len;
    unsigned int         at;
    int                  ok;
};

static unsigned int cur_u8(struct cur *c)
{
    if (c->at + 1 > c->len) { c->ok = 0; return 0; }
    return c->p[c->at++];
}

static unsigned int cur_u16(struct cur *c)
{
    unsigned int lo, hi;
    if (c->at + 2 > c->len) { c->ok = 0; c->at = c->len; return 0; }
    lo = c->p[c->at++];
    hi = c->p[c->at++];
    return lo | (hi << 8);
}

static const unsigned char *cur_block(struct cur *c, unsigned int n)
{
    const unsigned char *r;
    if (c->at + n > c->len) { c->ok = 0; c->at = c->len; return 0; }
    r = c->p + c->at;
    c->at += n;
    return r;
}

/* ---- packing ------------------------------------------------------------ */

static void bit_set(unsigned char *bits, unsigned int i, int v)
{
    if (v)
        bits[i >> 3] |= (unsigned char)(1u << (i & 7u));
    else
        bits[i >> 3] &= (unsigned char)~(1u << (i & 7u));
}

static int bit_get(const unsigned char *bits, unsigned int i)
{
    return (bits[i >> 3] >> (i & 7u)) & 1u;
}

static void put16(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v & 0xffu);
    p[1] = (unsigned char)((v >> 8) & 0xffu);
}

static unsigned int get16(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

/*
 * A rotate-and-xor checksum over everything the magic vouches for. A plain sum
 * would not notice two bytes swapping places, which is exactly the shape of the
 * corruption a partly-programmed card produces.
 */
static unsigned char card_sum(const unsigned char *p)
{
    unsigned int s = 0x5Au, i;

    for (i = 3; i < CARD_USER_SIZE; i++) {
        s = ((s << 1) | (s >> 7)) & 0xffu;
        s ^= p[i];
    }
    s ^= p[C_VERSION];

    return (unsigned char)(s & 0xffu);
}

/*
 * The engine's stream in, 96 card bytes out. Returns 0 and bumps
 * save_pack_overflows if the state does not fit — which today can only mean
 * more than MAX_EXC game flags holding a value other than 0 or 1.
 */
static int save_pack(const unsigned char *stream, unsigned int len,
                     unsigned char *out)
{
    struct cur c;
    const unsigned char *flags, *holo, *inv;
    unsigned int i, nexc = 0;
    unsigned int numcube, chapitre, comport, life, gold, mlevel, mpoint;
    unsigned int cloverbox, fuel, clover, weapon, sx, sy, sz, beta, genbody;

    c.p = stream; c.len = len; c.at = 0; c.ok = 1;

    cur_u8(&c);                                     /* version */
    while (c.ok && cur_u8(&c) != 0)                 /* name, NUL-terminated */
        { }

    if (cur_u8(&c) != N_FLAGS)  return 0;
    flags = cur_block(&c, N_FLAGS);

    numcube  = cur_u8(&c);
    chapitre = cur_u8(&c);
    comport  = cur_u8(&c);
    life     = cur_u8(&c);
    gold     = cur_u16(&c);
    mlevel   = cur_u8(&c);
    mpoint   = cur_u8(&c);
    cloverbox= cur_u8(&c);
    sx       = cur_u16(&c);
    sy       = cur_u16(&c);
    sz       = cur_u16(&c);
    beta     = cur_u16(&c);
    genbody  = cur_u8(&c);

    if (cur_u8(&c) != N_HOLO)   return 0;
    holo = cur_block(&c, N_HOLO);

    fuel = cur_u8(&c);

    if (cur_u8(&c) != N_INV)    return 0;
    inv = cur_block(&c, N_INV);

    clover = cur_u8(&c);
    weapon = cur_u16(&c);

    if (!c.ok || !flags || !holo || !inv)
        return 0;

    memset(out, 0, CARD_USER_SIZE);

    for (i = 0; i < N_FLAGS; i++) {
        bit_set(out + C_FLAGS, i, flags[i] != 0);

        if (flags[i] > 1) {
            if (nexc >= MAX_EXC) {
                save_pack_overflows++;
                return 0;
            }
            out[C_EXC + nexc * 2 + 0] = (unsigned char)i;
            out[C_EXC + nexc * 2 + 1] = flags[i];
            nexc++;
        }
    }

    if (nexc > save_flag_exceptions)
        save_flag_exceptions = nexc;

    out[C_NEXC] = (unsigned char)nexc;

    for (i = 0; i < N_HOLO; i++)
        bit_set(out + C_HOLO, i, (holo[i] & HOLO_SET) != 0);

    for (i = 0; i < N_INV; i++)
        bit_set(out + C_INV, i, inv[i] != 0);

    out[C_NUMCUBE]    = (unsigned char)numcube;
    out[C_CHAPITRE]   = (unsigned char)chapitre;
    out[C_COMPORT]    = (unsigned char)comport;
    out[C_LIFE]       = (unsigned char)life;
    out[C_MAGICLEVEL] = (unsigned char)mlevel;
    out[C_MAGICPOINT] = (unsigned char)mpoint;
    out[C_CLOVERBOX]  = (unsigned char)cloverbox;
    out[C_CLOVER]     = (unsigned char)clover;
    out[C_FUEL]       = (unsigned char)fuel;
    out[C_GENBODY]    = (unsigned char)genbody;

    put16(out + C_GOLD,   gold);
    put16(out + C_WEAPON, weapon);
    put16(out + C_STARTX, sx);
    put16(out + C_STARTY, sy);
    put16(out + C_STARTZ, sz);
    put16(out + C_BETA,   beta);

    out[C_VERSION] = CARD_FORMAT;
    out[C_SUM]     = card_sum(out);
    out[C_MAGIC]   = CARD_MAGIC;

    return 1;
}

/*
 * 96 card bytes in, an engine stream out. The name is synthesised rather than
 * stored: the card holds one save and calling it CARD is what makes the load
 * list readable, and thirty bytes of player-typed name would cost a third of
 * the card to no purpose.
 */
static const char CARD_SAVE_NAME[] = "CARD";

static int save_unpack(const unsigned char *in, unsigned char *stream,
                       unsigned int *out_len)
{
    unsigned char flags[N_FLAGS], holo[N_HOLO], inv[N_INV];
    unsigned int i, nexc;
    unsigned char *w = stream;

    if (in[C_MAGIC] != CARD_MAGIC)      return 0;
    if (in[C_VERSION] != CARD_FORMAT)   return 0;
    if (in[C_SUM] != card_sum(in))      return 0;

    for (i = 0; i < N_FLAGS; i++)
        flags[i] = (unsigned char)bit_get(in + C_FLAGS, i);

    nexc = in[C_NEXC];
    if (nexc > MAX_EXC)
        return 0;

    for (i = 0; i < nexc; i++) {
        unsigned int idx = in[C_EXC + i * 2 + 0];
        if (idx < N_FLAGS)
            flags[idx] = in[C_EXC + i * 2 + 1];
    }

    for (i = 0; i < N_HOLO; i++)
        holo[i] = (unsigned char)(bit_get(in + C_HOLO, i) ? HOLO_SET : 0u);

    for (i = 0; i < N_INV; i++)
        inv[i] = (unsigned char)bit_get(in + C_INV, i);

    /* Now write the stream in exactly the order SaveGameWithName() would. */
    *w++ = 3;                                   /* NumVersion */
    memcpy(w, CARD_SAVE_NAME, sizeof(CARD_SAVE_NAME));
    w += sizeof(CARD_SAVE_NAME);                /* includes the NUL */

    *w++ = (unsigned char)N_FLAGS;
    memcpy(w, flags, N_FLAGS);  w += N_FLAGS;

    *w++ = in[C_NUMCUBE];
    *w++ = in[C_CHAPITRE];
    *w++ = in[C_COMPORT];
    *w++ = in[C_LIFE];
    put16(w, get16(in + C_GOLD));       w += 2;
    *w++ = in[C_MAGICLEVEL];
    *w++ = in[C_MAGICPOINT];
    *w++ = in[C_CLOVERBOX];
    put16(w, get16(in + C_STARTX));     w += 2;
    put16(w, get16(in + C_STARTY));     w += 2;
    put16(w, get16(in + C_STARTZ));     w += 2;
    put16(w, get16(in + C_BETA));       w += 2;
    *w++ = in[C_GENBODY];

    *w++ = (unsigned char)N_HOLO;
    memcpy(w, holo, N_HOLO);    w += N_HOLO;

    *w++ = in[C_FUEL];

    *w++ = (unsigned char)N_INV;
    memcpy(w, inv, N_INV);      w += N_INV;

    *w++ = in[C_CLOVER];
    put16(w, get16(in + C_WEAPON));     w += 2;

    *out_len = (unsigned int)(w - stream);
    return 1;
}

/* ---- the card ----------------------------------------------------------- */

void save_init(void)
{
    int i;

    for (i = 0; i < MAX_SAVE_OPEN; i++)
        handles[i].used = 0;

    ram.len = 0;
    ram.valid = 0;
    card.len = 0;
    card.valid = 0;
    ram_gen = 0;
    saved_gen = 0;

    save_card_sync();
}

int save_card_present(void)
{
    return card_present();
}

int save_card_sync(void)
{
    unsigned char raw[CARD_USER_SIZE];

    card.valid = 0;
    card.len = 0;

    /* REQA first: it costs a millisecond and it is what keeps opening a load
     * list with no card on the reader from costing a third of a second. */
    if (!card_present())
        return 0;

    if (!card_read_user(raw))
        return 0;

    if (!save_unpack(raw, card.data, &card.len)) {
        card.len = 0;
        return 0;
    }

    card.valid = 1;
    return 1;
}

int save_to_card(void)
{
    unsigned char want[CARD_USER_SIZE];
    int steps;

    if (!ram.valid) {
        save_card_failures++;
        return 0;
    }

    if (!save_pack(ram.data, ram.len, want)) {
        save_card_failures++;
        return 0;
    }

    steps = card_write_begin(want);
    if (steps < 0) {
        save_card_failures++;
        return 0;
    }

    while (card_write_step() > 0)
        { }

    if (!card_write_finish()) {
        save_card_failures++;
        card.valid = 0;                 /* the magic is cleared: it really is
                                         * empty now, not stale */
        card.len = 0;
        return 0;
    }

    /* Keep the CARD.LBA slot in step with what was just programmed, without
     * paying for another whole-card read. */
    if (save_unpack(want, card.data, &card.len))
        card.valid = 1;

    saved_gen = ram_gen;
    save_card_saves++;
    return 1;
}

int save_ram_valid(void)  { return ram.valid; }
int save_card_valid(void) { return card.valid; }

int save_unsaved_progress(void)
{
    return ram.valid && ram_gen != saved_gen;
}

/* ---- the virtual files -------------------------------------------------- */

int save_open(const char *name, int for_write)
{
    struct slot *s = slot_for(name);
    int h;

    if (!s)
        return -1;

    /* The card is read-only through this interface on purpose. Writing it
     * takes up to two seconds and can fail, and fclose() has nowhere to put
     * either fact; save_to_card() is the way in. */
    if (for_write && s == &card)
        return -1;

    if (!for_write && !s->valid)
        return -1;

    for (h = 0; h < MAX_SAVE_OPEN; h++) {
        if (handles[h].used)
            continue;

        handles[h].used = 1;
        handles[h].s = s;
        handles[h].pos = 0;
        handles[h].writing = for_write;

        if (for_write) {
            s->len = 0;
            s->valid = 0;               /* not a save until it is closed */
        }
        return h;
    }
    return -1;
}

static struct save_handle *handle_of(int h)
{
    if (h < 0 || h >= MAX_SAVE_OPEN || !handles[h].used)
        return 0;
    return &handles[h];
}

int save_read(int h, void *buf, unsigned int len)
{
    struct save_handle *sh = handle_of(h);
    unsigned int left;

    if (!sh || sh->writing)
        return -1;

    if (sh->pos >= sh->s->len)
        return 0;

    left = sh->s->len - sh->pos;
    if (len > left)
        len = left;

    memcpy(buf, sh->s->data + sh->pos, len);
    sh->pos += len;
    return (int)len;
}

int save_write(int h, const void *buf, unsigned int len)
{
    struct save_handle *sh = handle_of(h);

    if (!sh || !sh->writing)
        return -1;

    /* A save that does not fit is a bug in the size of the buffer, not
     * something to wrap around: refuse the tail and let the short write be
     * visible to the caller. */
    if (sh->pos + len > SAVE_MAX_BYTES)
        len = (sh->pos < SAVE_MAX_BYTES) ? SAVE_MAX_BYTES - sh->pos : 0;

    if (len == 0)
        return 0;

    memcpy(sh->s->data + sh->pos, buf, len);
    sh->pos += len;
    if (sh->pos > sh->s->len)
        sh->s->len = sh->pos;

    return (int)len;
}

long save_lseek(int h, long off, int whence)
{
    struct save_handle *sh = handle_of(h);
    long base, pos;

    if (!sh)
        return -1;

    switch (whence) {
    case 0: base = 0; break;
    case 1: base = (long)sh->pos; break;
    case 2: base = (long)sh->s->len; break;
    default: return -1;
    }

    pos = base + off;
    if (pos < 0 || pos > (long)SAVE_MAX_BYTES)
        return -1;

    sh->pos = (unsigned int)pos;
    return pos;
}

long save_size(int h)
{
    struct save_handle *sh = handle_of(h);
    return sh ? (long)sh->s->len : -1;
}

int save_close(int h)
{
    struct save_handle *sh = handle_of(h);

    if (!sh)
        return -1;

    if (sh->writing && sh->s->len > 0) {
        sh->s->valid = 1;
        if (sh->s == &ram) {
            ram_gen++;
            save_ram_writes++;
        }
    }

    sh->used = 0;
    return 0;
}

const char *save_enumerate(int *index, unsigned long *size)
{
    for (;;) {
        struct slot *s;

        switch (*index) {
        case 0:  s = &ram;  break;
        case 1:  s = &card; break;
        default: return 0;
        }

        (*index)++;

        if (s->valid) {
            if (size)
                *size = s->len;
            return s->name;
        }
    }
}
