/*
 * torture.c — re-tests every code shape the hyperscan-homebrew bug list
 * declares broken on score-elf. Built with the OFFICIAL Sunplus toolchain
 * (GCC 4.2.1) at -mscore7 -mel -Os, i.e. the flags the SDK's own Makefiles use.
 *
 * Each function maps to a numbered bug in gcc-score-elf/bugs-and-workarounds.md.
 * The point is not to run it (that needs MAME) but to inspect the generated
 * asm for the specific defects that were documented.
 */

typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;

volatile u32 sink;

/* ---- Bug #3: the 5th argument (stack-passed) read at the wrong offset ---- */
int six_args(int a, int b, int c, int d, int e, int f)
{
    return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6;
}

int call_six_args(void)
{
    return six_args(1, 2, 3, 4, 5, 6);
}

/* ---- Bug #1 / #15: loop containing a call (jl); non-leaf saving r3 ---- */
static int leafy(int x) { return x * 3 + 1; }

int loop_with_calls(int n)
{
    int i, acc = 0;
    for (i = 0; i < n; i++)
        acc += leafy(i);
    return acc;
}

/* ---- Bug #15 minimal shape: non-leaf, saves r3, internal jl, tail branch ---- */
int nonleaf_with_jl(int a, int b)
{
    int t = leafy(a);
    sink = t;
    return six_args(t, b, 1, 2, 3, 4);
}

/* ---- Bug #7: division by a non-power-of-2 (calls __divsi3 via jl) ---- */
int divide_in_loop(int n)
{
    int i, acc = 0;
    for (i = 1; i <= n; i++)
        acc += n / i;
    return acc;
}

/* ---- Bug #11: small copy loop silently skipping an iteration ---- */
static int dst_a[8], dst_b[8];
void copy8(const int *sa, const int *sb)
{
    int i;
    for (i = 0; i < 8; i++) {
        dst_a[i] = sa[i];
        dst_b[i] = sb[i];
    }
}

/* ---- Bug #9: if/else chain with 5+ cases turned into a jump table ---- */
const char *menu_pick(int sel)
{
    switch (sel) {
    case 0: return "SIGNAL TEST";
    case 1: return "RFID VERIFY";
    case 2: return "3D STRESS TEST";
    case 3: return "AUDIO CHECK";
    case 4: return "PPU PARALLAX";
    case 5: return "CD SERVO";
    case 6: return "EXIT";
    default: return "?";
    }
}

/* ---- Bug #10: string-literal CSE across a ternary in a redraw path ---- */
extern void fb_puts(int x, int y, const char *s);
void redraw_menu(int sel)
{
    int i;
    for (i = 0; i < 7; i++)
        fb_puts(0, i, (sel == i) ? "> " : "  ");
    fb_puts(0, 8, (sel == 3) ? "> AUDIO CHECK" : "  AUDIO CHECK");
}

/* ---- Bug #16: constant stores at goto targets / across loop bodies ---- */
volatile u32 BUF_CTRL;
int draw_idx;

void flip_buffers(int frames)
{
    int i;
again:
    BUF_CTRL = 0;
    for (i = 0; i < frames; i++) {
        sink = i * 7 + draw_idx;
        draw_idx ^= 1;
    }
    if (frames > 100) { frames >>= 1; goto again; }
    BUF_CTRL = 1;
}

/* ---- byte-oriented data: the thing --reverse-bytes=4 destroys ---- */
const u8 font_row[16] = { 0x3C,0x42,0x81,0xA5,0x81,0xBD,0x42,0x3C,
                          0x00,0x18,0x24,0x42,0x99,0x42,0x24,0x18 };
const char *const names[3] = { "TWINSEN", "ZOE", "FUNFROCK" };

u8 read_font(int i) { return font_row[i & 15]; }
const char *read_name(int i) { return names[i % 3]; }

/* ---- struct by value + pointer chasing (ordinary engine code) ---- */
typedef struct { int x, y, z; u16 flags; u8 pal[4]; } actor_t;
static actor_t actors[8];

int sum_actor(actor_t a) { return a.x + a.y + a.z + a.flags + a.pal[0]; }

int walk_actors(int n)
{
    int i, acc = 0;
    for (i = 0; i < n && i < 8; i++)
        acc += sum_actor(actors[i]);
    return acc;
}

int main(void)
{
    sink = call_six_args();
    sink += loop_with_calls(10);
    sink += nonleaf_with_jl(3, 4);
    sink += divide_in_loop(7);
    copy8(dst_b, dst_a);
    sink += (u32)menu_pick(4)[0];
    redraw_menu(3);
    flip_buffers(200);
    sink += read_font(5) + (u32)read_name(2)[0];
    sink += walk_actors(8);
    return (int)sink;
}
