/*
 * test_save.c — the 96-byte card format, checked on the host.
 *
 * Built and run with the host compiler, not the score toolchain: the packer is
 * pure byte shuffling, and the one thing that must never happen to it — losing
 * part of a saved game quietly — is exactly the kind of bug that an emulator
 * run does not surface, because the game carries on looking fine and the loss
 * only shows up hours later in whichever scene needed the flag.
 *
 *   gcc -Wall -Wextra -O2 -o test_save tests/save/test_save.c && ./test_save
 *
 * src/save.c is #included rather than linked so that save_pack() and
 * save_unpack(), which are static and should stay that way, can be reached.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the card driver, stubbed ------------------------------------------- */
/* Nothing here exercises the wire protocol; that needs MAME. */

unsigned int card_transactions, card_bytes_written, card_errors, card_rall_fallbacks;

static unsigned char fake_card[96];

void card_init(void) { }
unsigned int card_calibration(void) { return 8; }
int  card_present(void) { return 1; }
int  card_read_user(unsigned char *b) { memcpy(b, fake_card, sizeof(fake_card)); return 1; }
const unsigned char *card_uid_bytes(void) { static const unsigned char u[4] = {0,0,0,0}; return u; }
int  card_write_begin(const unsigned char *w) { (void)w; return 0; }
int  card_write_step(void) { return 0; }
int  card_write_finish(void) { return 1; }

#include "../../src/save.c"

/* ---- a synthetic engine autosave ---------------------------------------- */
/*
 * Byte for byte what SaveGameWithName(name, 1) emits. Written out here rather
 * than captured from a run so that the expectations are readable, and so that
 * a change to the engine's save order fails this test instead of silently
 * shifting the card layout.
 */
struct state {
    unsigned char flags[255];
    unsigned char holo[150];
    unsigned char inv[28];
    unsigned int  numcube, chapitre, comport, life, gold;
    unsigned int  mlevel, mpoint, cloverbox, clover, fuel, weapon;
    unsigned int  sx, sy, sz, beta, genbody;
};

static unsigned int build(const struct state *s, const char *name,
                          unsigned char *out)
{
    unsigned char *w = out;

    *w++ = 3;
    strcpy((char *)w, name);
    w += strlen(name) + 1;

    *w++ = 255;
    memcpy(w, s->flags, 255); w += 255;

    *w++ = (unsigned char)s->numcube;
    *w++ = (unsigned char)s->chapitre;
    *w++ = (unsigned char)s->comport;
    *w++ = (unsigned char)s->life;
    *w++ = (unsigned char)(s->gold & 0xff);
    *w++ = (unsigned char)(s->gold >> 8);
    *w++ = (unsigned char)s->mlevel;
    *w++ = (unsigned char)s->mpoint;
    *w++ = (unsigned char)s->cloverbox;
    *w++ = (unsigned char)(s->sx & 0xff);   *w++ = (unsigned char)(s->sx >> 8);
    *w++ = (unsigned char)(s->sy & 0xff);   *w++ = (unsigned char)(s->sy >> 8);
    *w++ = (unsigned char)(s->sz & 0xff);   *w++ = (unsigned char)(s->sz >> 8);
    *w++ = (unsigned char)(s->beta & 0xff); *w++ = (unsigned char)(s->beta >> 8);
    *w++ = (unsigned char)s->genbody;

    *w++ = 150;
    memcpy(w, s->holo, 150); w += 150;

    *w++ = (unsigned char)s->fuel;

    *w++ = 28;
    memcpy(w, s->inv, 28); w += 28;

    *w++ = (unsigned char)s->clover;
    *w++ = (unsigned char)(s->weapon & 0xff);
    *w++ = (unsigned char)(s->weapon >> 8);

    return (unsigned int)(w - out);
}

/* ---- harness ------------------------------------------------------------ */

static int failures;
static int checks;

static void check(int cond, const char *what)
{
    checks++;
    if (!cond) {
        printf("FAIL  %s\n", what);
        failures++;
    }
}

static void check_eq(unsigned int got, unsigned int want, const char *what)
{
    checks++;
    if (got != want) {
        printf("FAIL  %s: got %u, want %u\n", what, got, want);
        failures++;
    }
}

/* Parse a rebuilt stream back into a state, so the comparison is field by
 * field rather than a memcmp that cannot say what moved. */
static int parse(const unsigned char *p, unsigned int len, struct state *s,
                 char *name)
{
    struct cur c;
    const unsigned char *b;

    c.p = p; c.len = len; c.at = 0; c.ok = 1;

    if (cur_u8(&c) != 3) return 0;

    {
        char *n = name;
        unsigned int ch;
        while ((ch = cur_u8(&c)) != 0 && c.ok)
            *n++ = (char)ch;
        *n = 0;
    }

    if (cur_u8(&c) != 255) return 0;
    b = cur_block(&c, 255); if (!b) return 0;
    memcpy(s->flags, b, 255);

    s->numcube   = cur_u8(&c);
    s->chapitre  = cur_u8(&c);
    s->comport   = cur_u8(&c);
    s->life      = cur_u8(&c);
    s->gold      = cur_u16(&c);
    s->mlevel    = cur_u8(&c);
    s->mpoint    = cur_u8(&c);
    s->cloverbox = cur_u8(&c);
    s->sx        = cur_u16(&c);
    s->sy        = cur_u16(&c);
    s->sz        = cur_u16(&c);
    s->beta      = cur_u16(&c);
    s->genbody   = cur_u8(&c);

    if (cur_u8(&c) != 150) return 0;
    b = cur_block(&c, 150); if (!b) return 0;
    memcpy(s->holo, b, 150);

    s->fuel = cur_u8(&c);

    if (cur_u8(&c) != 28) return 0;
    b = cur_block(&c, 28); if (!b) return 0;
    memcpy(s->inv, b, 28);

    s->clover = cur_u8(&c);
    s->weapon = cur_u16(&c);

    return c.ok;
}

int main(void)
{
    unsigned char stream[SAVE_MAX_BYTES], back[SAVE_MAX_BYTES];
    unsigned char packed[CARD_USER_SIZE];
    struct state in, out;
    char name[64];
    unsigned int len, backlen, i;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));

    /* A mid-game-shaped state: most flags off, a scattering on, a couple of
     * them holding a count rather than a boolean. */
    for (i = 0; i < 255; i += 7)
        in.flags[i] = 1;
    in.flags[3]   = 1;
    in.flags[17]  = 42;         /* not a boolean */
    in.flags[200] = 255;        /* nor this */

    /* Every value HOLOMAP.C can leave behind: unvisited, "arrow visible", and
     * the 64 that ClrHoloPos() writes and nothing ever reads. */
    for (i = 0; i < 150; i += 5)
        in.holo[i] = 1 + 128;
    in.holo[7]  = 64;
    in.holo[11] = 64;

    for (i = 0; i < 28; i += 3)
        in.inv[i] = 1;

    in.numcube = 42;  in.chapitre = 2;  in.comport = 3;   in.life = 47;
    in.gold = 1234;   in.mlevel = 3;    in.mpoint = 60;   in.cloverbox = 5;
    in.clover = 4;    in.fuel = 90;     in.weapon = 0x1234;
    in.sx = 0x1a2b;   in.sy = 0x0300;   in.sz = 0xfffe;   in.beta = 0x0200;
    in.genbody = 1;

    len = build(&in, "AUTOSAVE", stream);
    printf("engine autosave stream: %u bytes -> %u on the card\n",
           len, (unsigned)CARD_USER_SIZE);

    check(save_pack(stream, len, packed), "pack a normal state");
    check_eq(packed[C_MAGIC], 0x4C, "magic");
    check_eq(packed[C_NEXC], 2, "exception count");

    check(save_unpack(packed, back, &backlen), "unpack it again");
    check(parse(back, backlen, &out, name), "reparse the rebuilt stream");

    check(strcmp(name, "CARD") == 0, "the rebuilt save is named CARD");

    checks++;
    for (i = 0; i < 255; i++)
        if (in.flags[i] != out.flags[i]) {
            printf("FAIL  flag %u: got %u, want %u\n",
                   i, out.flags[i], in.flags[i]);
            failures++;
            break;
        }

    /* 64 is not preserved and must not be: it is written by ClrHoloPos() and
     * the only read of it in HOLOMAP.C is commented out. What matters is that
     * it comes back as "no arrow", the same thing 64 means today. */
    checks++;
    for (i = 0; i < 150; i++) {
        unsigned int want = (in.holo[i] & (1 + 128)) ? (1 + 128) : 0;
        if (out.holo[i] != want) {
            printf("FAIL  holo %u: got %u, want %u\n", i, out.holo[i], want);
            failures++;
            break;
        }
    }

    checks++;
    for (i = 0; i < 28; i++)
        if ((in.inv[i] != 0) != (out.inv[i] != 0)) {
            printf("FAIL  inventory %u\n", i);
            failures++;
            break;
        }

    check_eq(out.numcube,   in.numcube,   "NumCube");
    check_eq(out.chapitre,  in.chapitre,  "Chapitre");
    check_eq(out.comport,   in.comport,   "Comportement");
    check_eq(out.life,      in.life,      "LifePoint");
    check_eq(out.gold,      in.gold,      "NbGoldPieces");
    check_eq(out.mlevel,    in.mlevel,    "MagicLevel");
    check_eq(out.mpoint,    in.mpoint,    "MagicPoint");
    check_eq(out.cloverbox, in.cloverbox, "NbCloverBox");
    check_eq(out.clover,    in.clover,    "NbFourLeafClover");
    check_eq(out.fuel,      in.fuel,      "Fuel");
    check_eq(out.weapon,    in.weapon,    "Weapon");
    check_eq(out.sx,        in.sx,        "SceneStartX");
    check_eq(out.sy,        in.sy,        "SceneStartY");
    check_eq(out.sz,        in.sz,        "SceneStartZ");
    check_eq(out.beta,      in.beta,      "Beta");
    check_eq(out.genbody,   in.genbody,   "GenBody");

    /* A save that cannot be represented must be refused, not truncated. */
    {
        unsigned char probe[CARD_USER_SIZE];
        struct state big = in;          /* already holds two: 17 and 200 */

        for (i = 0; i < 6; i++)
            big.flags[20 + i] = (unsigned char)(2 + i);

        len = build(&big, "AUTOSAVE", stream);
        check(!save_pack(stream, len, probe), "refuse 8 non-boolean flags");
        check_eq(save_pack_overflows, 1, "and count the refusal");

        big.flags[25] = 1;                              /* back down to 7 */
        len = build(&big, "AUTOSAVE", stream);
        check(save_pack(stream, len, probe), "accept 7 of them");
        check_eq(save_flag_exceptions, 7, "high-water mark");
    }

    /* Corruption must not read as a save. */
    {
        unsigned char torn[CARD_USER_SIZE];
        unsigned int dummy;

        len = build(&in, "AUTOSAVE", stream);
        save_pack(stream, len, torn);

        torn[C_MAGIC] = 0;
        check(!save_unpack(torn, back, &dummy), "no magic, no save");

        save_pack(stream, len, torn);
        torn[C_GOLD] ^= 0x40;
        check(!save_unpack(torn, back, &dummy), "a flipped byte fails the sum");

        save_pack(stream, len, torn);
        torn[C_FLAGS + 4] ^= 0x01;
        check(!save_unpack(torn, back, &dummy), "a flipped flag bit too");

        save_pack(stream, len, torn);
        torn[C_VERSION] = 99;
        check(!save_unpack(torn, back, &dummy), "a future format is not read");
    }

    /* A truncated stream must not produce a confident-looking card. */
    {
        unsigned char probe[CARD_USER_SIZE];

        len = build(&in, "AUTOSAVE", stream);
        check(!save_pack(stream, len - 1, probe), "refuse a short stream");
        check(!save_pack(stream, 4, probe), "refuse a tiny one");
    }

    if (failures == 0)
        printf("ok - %d checks, card format round-trips\n", checks);
    else
        printf("%d failure(s) out of %d checks\n", failures, checks);

    return failures != 0;
}
