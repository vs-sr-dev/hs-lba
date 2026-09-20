#ifndef HS_HEAP_H
#define HS_HEAP_H

/* Heap accounting, from src/syscalls.c. Bytes committed by sbrk and bytes left
 * before the ceiling in src/memmap.h — not a malloc-level figure, but enough to
 * tell whether a load is going to fit. */
unsigned int heap_base(void);
unsigned int heap_used(void);
unsigned int heap_free(void);

#endif
