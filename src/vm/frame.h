#ifndef FRAME_H
#define FRAME_H

#include <list.h>
#include "threads/synch.h"

struct frame {
    struct list_elem list_elem;
    void *kpage;
    struct page_entry *pe;
    struct lock l;
};

void frame_init (void);
void frame_alloc (struct page_entry *pe);
void frame_dealloc (struct page_entry *);

#endif /* vm/frame.h */
