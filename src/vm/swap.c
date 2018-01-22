#include "vm/swap.h"
#include <bitmap.h>
#include "devices/block.h"
#include "threads/synch.h"
#include "threads/vaddr.h"

struct bitmap *used_map;
struct block *swap;
struct lock lock;

void swap_init (void)
{
  swap = block_get_role (BLOCK_SWAP);
  used_map = bitmap_create (block_size (swap) * BLOCK_SECTOR_SIZE / PGSIZE);
  lock_init (&lock);
}

size_t swap_write_slot (void *page)
{
  lock_acquire (&lock);
  size_t slot_id = bitmap_scan_and_flip (used_map, 0, 1, false);
  for (size_t i = 0; i < PGSIZE/BLOCK_SECTOR_SIZE; i++)
      block_write (swap, slot_id*(PGSIZE/BLOCK_SECTOR_SIZE) + i, (uint8_t *)page + i*BLOCK_SECTOR_SIZE);
  lock_release (&lock);

  return slot_id;
}

void swap_read_slot (void *page, size_t slot_id)
{
  lock_acquire (&lock);
  bitmap_reset (used_map, slot_id);
  for (size_t i = 0; i < PGSIZE/BLOCK_SECTOR_SIZE; i++)
      block_read (swap, slot_id*(PGSIZE/BLOCK_SECTOR_SIZE) + i, (uint8_t *)page + i*BLOCK_SECTOR_SIZE);
  lock_release (&lock);
}