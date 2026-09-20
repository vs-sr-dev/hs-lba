/* hqr.h — LBA resource-file reader, in-memory variant.
 *
 * The DOS/DS readers seek around the file with stdio. Here the whole HQR is
 * already resident: with 16 MB of SDRAM against 8.5 MB of core resources, the
 * plan is one big sequential read at boot and no CD access during play, so a
 * reader that parses a buffer is both simpler and the shape we actually want.
 */
#ifndef HS_HQR_H
#define HS_HQR_H

typedef unsigned int   hqr_u32;
typedef unsigned short hqr_u16;
typedef unsigned char  hqr_u8;

/* Entry count, read from the offset table. */
int hqr_num_entries(const hqr_u8 *hqr);

/* Uncompressed size of an entry, 0 if the index is out of range. */
hqr_u32 hqr_entry_size(const hqr_u8 *hqr, int index);

/* Decode an entry into dest; returns bytes written, 0 on failure. */
hqr_u32 hqr_load(const hqr_u8 *hqr, int index, hqr_u8 *dest);

#endif
