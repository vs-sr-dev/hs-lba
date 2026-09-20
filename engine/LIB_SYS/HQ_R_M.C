#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>

#include "lib_sys/adeline.h"
#include "lib_sys/lib_sys.h"

#define RECOVER_AREA 500

/*──────────────────────────────────────────────────────────────────────────*/

#pragma pack(push, 1) /* PORT: on-disk struct (10 bytes packed, see HQ_RESS.C) */
typedef struct
{
	ULONG SizeFile;
	ULONG CompressedSizeFile;
	WORD CompressMethod; /* 0 stored */
						 /* 1 LZS */
} PORT_PACKED T_HEADER;
#pragma pack(pop)

void Expand(void *ptrsourcecomp, void *ptrblocdest, ULONG sizefile);

/*──────────────────────────────────────────────────────────────────────────*/

LONG HQRM_Load(UBYTE *name, UWORD index, void **ptrptr)
{
	FILE *handle;
	UWORD nbbloc;
	ULONG buffer;
	ULONG seekindex;
	//	UBYTE		*ptrbloc ;
	UBYTE *ptrdecomp;
	T_HEADER header;

	handle = OpenRead(name);
	if (!handle)
		return FALSE;

	Read(handle, &buffer, 4L);
	nbbloc = (UWORD)(buffer / 4L);

	/*
	 * PORT: all three failure exits below used to return without Close(), and
	 * on DOS that was survivable — the C library had file handles to spare and
	 * the process was about to print an error and quit anyway.
	 *
	 * Here it turns a local failure into a total one. src/fs.c keeps eight
	 * handles. HQ_Mem is a 400 KB bump allocator that is never given back until
	 * the next scene change, so once a scene has loaded enough bodies the
	 * HQM_Alloc below starts failing — and each failure used to leak a handle.
	 * Eight of them and fs_open() refuses *every* file from then on: no bodies,
	 * no sprites, no animations, and a room that draws with its doors and upper
	 * layers missing rather than an error. The pool being too small is a
	 * separate question; losing the filesystem over it is not.
	 */
	if (index >= nbbloc)
	{
		Close(handle);
		return FALSE;
	}

	Seek(handle, index * 4L, SEEK_START);
	Read(handle, &seekindex, 4L);

	Seek(handle, seekindex, SEEK_START);
	Read(handle, &header, sizeof(header));

	if (!HQM_Alloc(header.SizeFile, ptrptr)) // il y a deja 500 de plus
	{
		Close(handle);
		return FALSE;
	}

	switch (header.CompressMethod)
	{
	case 0: /* Stored */
		Read(handle, *ptrptr, header.SizeFile);
		break;

	case 1: /* LZS */
		ptrdecomp = (UBYTE *)(*ptrptr) + header.SizeFile - header.CompressedSizeFile + RECOVER_AREA;
		Read(handle, ptrdecomp, header.CompressedSizeFile);
		Expand(ptrdecomp, *ptrptr, header.SizeFile);
		break;

	default:
		HQM_Free(*ptrptr);
		Close(handle);
		return FALSE; /* UnKnown version */
	}

	Close(handle);

	return header.SizeFile;
}
