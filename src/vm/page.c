#include "vm/page.h"
#include "vm/swap.h"
#include "threads/vaddr.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "userprog/pagedir.h"
#include <hash.h>
#include <string.h>
#include "userprog/syscall.h"
#include "threads/palloc.h"

#define STK_TOP PHYS_BASE
#define STK_BOTTOM PHYS_BASE - (1 << 23)

struct page_entry *get_page_entry (struct thread *, void *);
unsigned page_entry_hash (const struct hash_elem *, void *);
bool page_entry_less (const struct hash_elem *, const struct hash_elem *, void *);

unsigned
page_entry_hash (const struct hash_elem *p_, void *aux UNUSED)
{
  const struct page_entry *pe = hash_entry (p_, struct page_entry, hash_elem);
  return hash_bytes (&pe->upage, sizeof pe->upage);
}

bool
page_entry_less (const struct hash_elem *a_, const struct hash_elem *b_,
           void *aux UNUSED)
{
  const struct page_entry *a = hash_entry (a_, struct page_entry, hash_elem);
  const struct page_entry *b = hash_entry (b_, struct page_entry, hash_elem);

  return a->upage < b->upage;
}

void
page_init (struct thread *t)
{
  hash_init (&t->page_entries, page_entry_hash, page_entry_less, NULL);
}

void page_add_file (void *upage, bool writable, struct file *file, size_t len, off_t off)
{
  struct page_entry *pe = malloc (sizeof (struct page_entry));
  pe->upage = upage;
  pe->writable = writable;
  pe->zero = false;
  pe->file_backing = true;
  pe->frame = NULL;
  pe->swap_slot_id = -1;
  lock_init (&pe->l);

  lock_acquire (&fslock);
  pe->file = file_reopen (file);
  lock_release (&fslock);

  pe->len = len;
  pe->off = off;

  struct thread *t = thread_current ();
  pe->thread = t;
  hash_insert (&t->page_entries, &pe->hash_elem);
}

void page_add_file_without_backing (void *upage, bool writable, struct file *file, size_t len, off_t off)
{
  struct page_entry *pe = malloc (sizeof (struct page_entry));
  pe->upage = upage;
  pe->writable = writable;
  pe->zero = false;
  pe->file_backing = false;
  pe->frame = NULL;
  pe->swap_slot_id = -1;
  lock_init (&pe->l);

  lock_acquire (&fslock);
  pe->file = file_reopen (file);
  lock_release (&fslock);

  pe->len = len;
  pe->off = off;

  struct thread *t = thread_current ();
  pe->thread = t;
  hash_insert (&t->page_entries, &pe->hash_elem);
}

void page_add_zero (void *upage, bool writable)
{
  struct page_entry *pe = malloc (sizeof (struct page_entry));
  pe->upage = upage;
  pe->writable = writable;
  pe->frame = NULL;
  pe->swap_slot_id = -1;
  lock_init(&pe->l);

  pe->file = NULL;
  pe->zero = true;
  
  struct thread *t = thread_current ();
  pe->thread = t;
  hash_insert (&t->page_entries, &pe->hash_elem);
}

void hash_remove_page (struct hash_elem *, void *);
void hash_remove_page (struct hash_elem *he, void *aux UNUSED)
{
  struct page_entry *pe = hash_entry (he, struct page_entry, hash_elem);
  frame_dealloc (pe);
  free (pe);
}

void page_destroy (void)
{
  hash_destroy (&thread_current ()->page_entries, hash_remove_page);
}

void page_remove (void *upage)
{
  struct thread *t = thread_current ();
  struct page_entry *pe = get_page_entry (t, upage);
  if (pe != NULL)
  {
    frame_dealloc (pe);
    hash_delete (&t->page_entries, &pe->hash_elem);
    free (pe);
  }
}

bool page_present (void *upage)
{
  struct thread *t = thread_current ();
  struct page_entry *pe = get_page_entry (t, upage);
  return pe != NULL;
}

struct page_entry *get_page_entry (struct thread *t, void *upage)
{
  struct page_entry pe = {.upage = upage};
  struct hash_elem *he = hash_find (&t->page_entries, &pe.hash_elem);
  if (he != NULL)
    return hash_entry (he, struct page_entry, hash_elem);
  return NULL;
}

bool page_handle_fault (void *fault_addr, void *esp)
{
  struct thread *t = thread_current ();
  void *upage = pg_round_down (fault_addr);
  
  struct page_entry *pe = get_page_entry (t, upage);

  if (pe == NULL)
    {
      uintptr_t diff = esp - fault_addr;
      if (fault_addr >= STK_BOTTOM && fault_addr < STK_TOP &&
        (fault_addr >= esp || ((diff & (diff - 1)) == 0  && diff <= 32)))
      {
        uint8_t *stack_page = palloc_get_page (PAL_USER | PAL_ZERO);
        if (stack_page != NULL) {
          pagedir_set_page (t->pagedir, upage, stack_page, true);
          return true;
        }
      }
    }
  
  if (pe != NULL)
    {
      frame_alloc (pe);
      return true;
    }
  return false;
}