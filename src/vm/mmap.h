#ifndef MMAP_H
#define MMAP_H

#include <hash.h>
#include "lib/user/syscall.h"
#include "threads/thread.h"

struct mmap_entry
  {
    struct hash_elem hash_elem;

    mapid_t id;
    void *upage;
    size_t page_count;
  };

void mmap_init (struct thread *);

mapid_t sys_mmap (int, void *);
void sys_munmap (mapid_t);

void mmap_destory (void);

#endif /* vm/mmap.h */
