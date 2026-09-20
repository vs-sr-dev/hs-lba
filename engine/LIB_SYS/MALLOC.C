#include <stdio.h>
#include <stdlib.h>

#include "ADELINE.H"
#include "LIB_SYS.H"

#ifdef DEBUG_MALLOC
LONG ModeTraceMalloc = FALSE;
#endif

/* Only compile these functions for non-DOS platforms */
/* DOS builds use the full DPMI implementation from LIB_SYS_DOS/MALLOC.C */
#ifndef __DOS__

/* PORT: Malloc(-1) is a *question*, not an allocation — "how much memory is
   there?" — and PERSO.C sizes the sample, animation and sprite caches from the
   answer. Returning 0 is not a harmless stub: every pool then falls to the DOS
   minimum (200K samples, 100K anims, 50K sprites) on a console with megabytes
   free, so the caches thrash. That is a resource evicted and re-read from the
   CD every time it is used, which is audible as a hitch on each new sound and
   is a hard failure when an animation the game is mid-way through using goes
   away. The engine's own clamps still apply on top of the honest figure. */
#if defined(PORT_HS)
extern unsigned int heap_free(void); /* src/syscalls.c */
#endif

void *Malloc(LONG lenalloc)
{
	void *ptr;

	if (lenalloc == -1)
	{
#if defined(PORT_HS)
		/* Leave a megabyte: the pools are not the only thing that allocates
		   after this, and the engine hands the whole answer to the caches. */
		unsigned int free = heap_free();

		return (void *)(free > (1024u * 1024u) ? free - 1024u * 1024u : 0u);
#else
		return 0; /* Query memory not supported */
#endif
	}

	ptr = malloc(lenalloc);

	if (ptr == NULL)
	{
		printf("ERROR: MemoryNotAlloc (Malloc): Size = %d\n", lenalloc);
	}

	return ptr;
}

void *SmartMalloc(LONG lenalloc)
{
	/* SmartMalloc is just an alias for Malloc */
	return Malloc(lenalloc);
}

void Free(void *buffer)
{
	if (buffer != NULL)
	{
		free(buffer);
	}
}

void *Mshrink(void *buffer, ULONG taille)
{
	void *new_buffer;

	if (buffer == NULL)
	{
		return NULL;
	}

	new_buffer = realloc(buffer, taille);
	return new_buffer ? new_buffer : buffer;
}

#endif /* !__DOS__ */
