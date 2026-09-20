/*----------------------------------------------------------------------------
 *  fastmem.h — short-run copy and fill for the rasterisers.
 *
 *  These exist because of what newlib's memcpy actually does on this target.
 *  Disassembled, it tests `(src | dst) & 3` and takes a 16-byte unrolled word
 *  loop only when *both* pointers are already word-aligned; anything else falls
 *  into a plain byte loop. That is the common case here and it is the expensive
 *  one:
 *
 *    - CopyBlock() (s_block.c) is the ClsBoxes background restore, one call per
 *      row of every dirty rectangle. Source and destination are the same column
 *      of two 640-wide buffers, so they always share an alignment phase and
 *      almost never start word-aligned — three rows in four were being copied a
 *      byte at a time. `memcpy <- CopyBlock` was 2.9% of the frame.
 *    - AffGraph() (graph_a.c) calls memcpy/memset once per RLE run, so 1 to 64
 *      bytes at a time, where the call itself is a large part of the cost.
 *
 *  FastCopy takes the word path whenever the two pointers share a phase, not
 *  only when that phase is zero — align the head by hand, then move words. That
 *  is the case libc gives up on and the case this code produces.
 *
 *  FastFill can always reach word alignment, because there is no source to stay
 *  in step with.
 *
 *  Both fall back to bytes for short runs, where the setup would cost more than
 *  it saves.
 *---------------------------------------------------------------------------*/
#ifndef HS_FASTMEM_H
#define HS_FASTMEM_H

/* Word accesses through a byte buffer. may_alias keeps the optimiser from
   assuming the two views cannot overlap — the buffers really are bytes and are
   really written both ways. */
typedef unsigned long __attribute__((__may_alias__)) FM_U32;

/* Below this the head/tail handling costs more than the word loop saves. */
#define FM_MIN_WORDS 16

static __inline__ void FastCopy(unsigned char *d, const unsigned char *s, long n)
{
	if (n >= FM_MIN_WORDS && ((((unsigned long)d) ^ ((unsigned long)s)) & 3UL) == 0UL)
	{
		FM_U32 *dw;
		const FM_U32 *sw;
		long w;

		while ((((unsigned long)d) & 3UL) != 0UL)
		{
			*d++ = *s++;
			n--;
		}

		dw = (FM_U32 *)d;
		sw = (const FM_U32 *)s;
		w = n >> 2;
		n &= 3;

		while (w--)
			*dw++ = *sw++;

		d = (unsigned char *)dw;
		s = (const unsigned char *)sw;
	}

	while (n--)
		*d++ = *s++;
}

static __inline__ void FastFill(unsigned char *d, unsigned char v, long n)
{
	if (n >= FM_MIN_WORDS)
	{
		FM_U32 *dw;
		FM_U32 p = (FM_U32)v * 0x01010101UL;
		long w;

		while ((((unsigned long)d) & 3UL) != 0UL)
		{
			*d++ = v;
			n--;
		}

		dw = (FM_U32 *)d;
		w = n >> 2;
		n &= 3;

		while (w--)
			*dw++ = p;

		d = (unsigned char *)dw;
	}

	while (n--)
		*d++ = v;
}

#endif
