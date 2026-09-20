/*
 * rotate_test.c — verifies the score rotate instructions.
 *
 * MAME's score7 core shipped every rotate (rol/ror/roli/rori and the carry
 * variants) as unemulated_op(), a fatal error. GCC 4.2.1 emits `rori`/`roli`
 * for any ordinary C rotate idiom, so this fires on innocuous code — HQR's
 * LZSS decompressor is the one that would have bitten us.
 *
 * Rather than trust a table of expected values, each result is checked against
 * a reference that rotates one bit at a time through a volatile accumulator,
 * which GCC cannot pattern-match back into a rotate instruction. So this
 * compares the CPU's rotate against first principles, with no host in the loop.
 */

#include "rotate_test.h"

typedef unsigned int u32;

/* Reference: one bit at a time, volatile so it stays shifts-and-ors. */
static u32 ref_rotr(u32 x, unsigned n)
{
    volatile u32 r = x;
    unsigned i;
    for (i = 0; i < (n & 31); i++)
        r = (r >> 1) | ((r & 1u) << 31);
    return r;
}

static u32 ref_rotl(u32 x, unsigned n)
{
    volatile u32 r = x;
    unsigned i;
    for (i = 0; i < (n & 31); i++)
        r = (r << 1) | ((r >> 31) & 1u);
    return r;
}

/* Under test.
 *
 * GCC 4.2.1's score backend only has an immediate rotate pattern, and it
 * canonicalises rotate-left-by-k into rotate-right-by-(32-k) — so compiling C
 * rotate idioms exercises `rori` and nothing else. The register-count forms and
 * `roli` are therefore driven from inline asm, otherwise three of the four
 * opcodes we patched would go untested. */
static u32 rotr_var(u32 x, unsigned n)
{
    u32 r;
    __asm__ ("ror %0, %1, %2" : "=r"(r) : "r"(x), "r"(n));
    return r;
}

static u32 rotl_var(u32 x, unsigned n)
{
    u32 r;
    __asm__ ("rol %0, %1, %2" : "=r"(r) : "r"(x), "r"(n));
    return r;
}

static u32 rotl_imm7(u32 x)
{
    u32 r;
    __asm__ ("roli %0, %1, 7" : "=r"(r) : "r"(x));
    return r;
}

#define ROTR_CONST(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define ROTL_CONST(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

int rotate_selftest(void)
{
    static const u32 patterns[] = {
        0x00000001u, 0x80000000u, 0xDEADBEEFu, 0x12345678u,
        0xFFFFFFFFu, 0x00000000u, 0xA5A5A5A5u, 0x0F0F0F0Fu
    };
    unsigned p, n;
    int fails = 0;

    for (p = 0; p < sizeof(patterns) / sizeof(patterns[0]); p++) {
        const u32 x = patterns[p];

        /* Register-count forms (ror / rol), every count 1..31. The count goes
         * through a volatile so GCC cannot constant-fold it back into an
         * immediate rotate — otherwise this would silently retest rori. */
        for (n = 1; n < 32; n++) {
            volatile unsigned vn = n;
            volatile u32 vx = x;
            if (rotr_var(vx, vn) != ref_rotr(x, n)) fails++;
            if (rotl_var(vx, vn) != ref_rotl(x, n)) fails++;
        }

        /* roli, which the compiler never picks on its own */
        if (rotl_imm7(x) != ref_rotl(x, 7)) fails++;

        /* constant-count forms: rori / roli */
        if (ROTR_CONST(x, 1)  != ref_rotr(x, 1))  fails++;
        if (ROTR_CONST(x, 7)  != ref_rotr(x, 7))  fails++;
        if (ROTR_CONST(x, 16) != ref_rotr(x, 16)) fails++;
        if (ROTR_CONST(x, 31) != ref_rotr(x, 31)) fails++;
        if (ROTL_CONST(x, 1)  != ref_rotl(x, 1))  fails++;
        if (ROTL_CONST(x, 3)  != ref_rotl(x, 3))  fails++;
        if (ROTL_CONST(x, 16) != ref_rotl(x, 16)) fails++;
        if (ROTL_CONST(x, 31) != ref_rotl(x, 31)) fails++;
    }

    return fails;
}
