#ifndef PAGE_H
#define PAGE_H

#include <hash.h>
#include "lib/user/syscall.h"
#include "filesys/file.h"
#include "threads/thread.h"
#include "threads/synch.h"

struct page_entry {
    struct hash_elem hash_elem;
    struct frame *frame;
    struct thread *thread;
    struct lock l;

    void *upage;

    bool writable;

    bool zero;

    bool file_backing;
    struct file *file;
    size_t len;
    off_t off;

    size_t swap_slot_id;
};

void page_init (struct thread *);

void page_writeout (struct thread *, struct page_entry *);
void page_add_file (void *, bool, struct file *, size_t, off_t);
void page_add_file_without_backing (void *, bool, struct file *, size_t, off_t);
void page_add_zero (void *, bool);
void page_remove (void *);
bool page_present (void *);
void page_destroy (void);
bool page_handle_fault (void *, void *);

#endif /* vm/page.h */
