/*
 * hqr.c — LBA resource-file reader (in-memory).
 *
 * On-disk layout, little endian, matching T_HEADER in the engine's HQ_RESS.C:
 *   u32 offset[i] for each entry; offset[0] doubles as the table size, so
 *   entry count = offset[0] / 4.
 *   At offset[i]: u32 size, u32 compressed_size, u16 method, then the data.
 *   method 0 = stored, 1 = LZSS.
 *
 * The 10-byte entry header is packed on disk and lands at an arbitrary offset,
 * so every field is assembled byte-wise: GCC would pad the struct to 12, and
 * score7 has no cheap unaligned 32-bit load.
 */

#include "hqr.h"

static hqr_u32 rd32(const hqr_u8 *p)
{
    return (hqr_u32)p[0] | ((hqr_u32)p[1] << 8) |
           ((hqr_u32)p[2] << 16) | ((hqr_u32)p[3] << 24);
}

static hqr_u16 rd16(const hqr_u8 *p)
{
    return (hqr_u16)(p[0] | (p[1] << 8));
}

/* LZSS: one control byte carries 8 flags, LSB first. flag=1 emits a literal;
 * flag=0 reads a 16-bit token of (distance << 4) | (len - 2) and copies from
 * already-written output, byte at a time because runs may self-overlap. */
static void hqr_expand(const hqr_u8 *src, hqr_u8 *dest, int count)
{
    while (count > 0) {
        int flags = *src++;
        int bit;

        for (bit = 0; bit < 8; bit++, flags >>= 1) {
            if (flags & 1) {
                *dest++ = *src++;
                if (--count == 0)
                    return;
            } else {
                hqr_u16 token = rd16(src);
                int len = (token & 0x0F) + 2;
                const hqr_u8 *back = dest - (token >> 4) - 1;

                src += 2;
                count -= len;
                while (len-- > 0)
                    *dest++ = *back++;
                if (count <= 0)
                    return;
            }
        }
    }
}

int hqr_num_entries(const hqr_u8 *hqr)
{
    return (int)(rd32(hqr) / 4);
}

static const hqr_u8 *hqr_entry(const hqr_u8 *hqr, int index)
{
    if (index < 0 || index >= hqr_num_entries(hqr))
        return 0;
    return hqr + rd32(hqr + index * 4);
}

hqr_u32 hqr_entry_size(const hqr_u8 *hqr, int index)
{
    const hqr_u8 *e = hqr_entry(hqr, index);
    return e ? rd32(e) : 0;
}

hqr_u32 hqr_load(const hqr_u8 *hqr, int index, hqr_u8 *dest)
{
    const hqr_u8 *e = hqr_entry(hqr, index);
    hqr_u32 size, i;
    hqr_u16 method;
    const hqr_u8 *data;

    if (!e)
        return 0;

    size   = rd32(e);
    method = rd16(e + 8);
    data   = e + 10;

    switch (method) {
    case 0:
        for (i = 0; i < size; i++)
            dest[i] = data[i];
        return size;
    case 1:
        hqr_expand(data, dest, (int)size);
        return size;
    default:
        return 0;
    }
}
