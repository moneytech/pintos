#include <stdbool.h>
#include "threads/malloc.h"
#include <limits.h>
#include "vm/frame.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "userprog/syscall.h"
#include "userprog/pagedir.h"
#include "vm/swap.h"
#include "vm/page.h"
#include <string.h>

struct list frametable;
struct lock ftlock;

struct list_elem *tick;

void
frame_init (void)
{
  list_init (&frametable);
  lock_init (&ftlock);
}

void
frame_alloc (struct page_entry *pe)
{
  struct frame *frame;
  void *page = palloc_get_page (PAL_USER);
  if (page == NULL)
  {
    lock_acquire (&ftlock);
    frame = list_entry (tick, struct frame, list_elem);
    tick = list_next (tick);
    if (tick == list_end (&frametable))
      tick = list_begin (&frametable);
    lock_release (&ftlock);

    lock_acquire (&frame->l);

    if (frame->pe != NULL)
      {
        struct page_entry *pe_evict = frame->pe;

        lock_acquire (&pe_evict->l);
        struct thread *t = pe->thread;

        bool dirty;
        lock_acquire (&t->pagedir_lock);
        dirty = pagedir_is_dirty (t->pagedir, pe_evict->upage);
        pagedir_clear_page (t->pagedir, pe_evict->upage);
        lock_release (&t->pagedir_lock);

        if (pe_evict->writable)
          {
            if (pe_evict->file && dirty && pe_evict->file_backing)
              {
                lock_acquire (&fslock);
                file_write_at (pe_evict->file, frame->kpage, pe_evict->len, pe_evict->off);
                lock_release (&fslock);
              }
            else
              pe_evict->swap_slot_id = swap_write_slot (frame->kpage);
          }
        
        pe_evict->frame = NULL;
        lock_release (&pe_evict->l);
      }

    page = frame->kpage;
  } else {
    frame = malloc (sizeof (struct frame));
    frame->kpage = page;
    lock_init (&frame->l);
    lock_acquire (&frame->l);
  }
  frame->pe = pe;

  memset (frame->kpage, 0, PGSIZE);

  lock_acquire (&pe->l);

  if (pe->swap_slot_id != -1)
    {
      swap_read_slot (frame->kpage, pe->swap_slot_id);
      pe->swap_slot_id = -1;
    }
  else if (pe->file)
    {
      lock_acquire (&fslock);
      file_read_at (pe->file, frame->kpage, pe->len, pe->off);
      lock_release (&fslock);
    }
  pe->frame = frame;

  struct thread *t = thread_current ();
  lock_acquire (&t->pagedir_lock);
  pagedir_set_page (t->pagedir, pe->upage, frame->kpage, pe->writable);
  lock_release (&t->pagedir_lock);
  
  lock_release (&pe->l);

  lock_release (&frame->l);

  lock_acquire (&ftlock);
  if (list_empty (&frametable))
    tick = &frame->list_elem;
  list_push_back (&frametable, &frame->list_elem);
  lock_release (&ftlock);
}

void
frame_dealloc (struct page_entry *pe)
{
  lock_acquire (&pe->l);

  struct frame *frame = pe->frame;
  if (frame != NULL)
    {
      lock_acquire (&frame->l);

      struct thread *t = pe->thread;

      bool present, dirty;
      lock_acquire (&t->pagedir_lock);
      present = pagedir_get_page (t->pagedir, pe->upage) != NULL;
      if (present) {
        dirty = pagedir_is_dirty (t->pagedir, pe->upage);
        //pagedir_clear_page (t->pagedir, pe->upage);
      }
      lock_release (&t->pagedir_lock);

      if (present && pe->file && pe->file_backing && pe->writable && dirty)
        {
          lock_acquire (&fslock);
          file_write_at (pe->file, frame->kpage, pe->len, pe->off);
          file_close (pe->file);
          lock_release (&fslock);
        }

      frame->pe = NULL;
      lock_release (&frame->l);
    }  

  pe->frame = NULL;
  lock_release (&pe->l);
}