#ifndef SWAP_H
#define SWAP_H

#include <stddef.h>

void swap_init (void);

size_t swap_write_slot (void *);
void swap_read_slot (void *, size_t);

#endif /* vm/swap.h */
