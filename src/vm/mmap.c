#include "vm/mmap.h"
#include "threads/thread.h"
#include "filesys/file.h"
#include "userprog/syscall.h"
#include "threads/malloc.h"
#include "userprog/pagedir.h"
#include "threads/vaddr.h"
#include "vm/page.h"
#include "userprog/process.h"

static unsigned mmap_entry_hash (const struct hash_elem *, void *);
static void hash_remove_mmap_entry (struct hash_elem *, void *);
static bool mmap_entry_less (const struct hash_elem *, const struct hash_elem *, void *);

static unsigned
mmap_entry_hash (const struct hash_elem *p_, void *aux UNUSED)
{
  const struct mmap_entry *me = hash_entry (p_, struct mmap_entry, hash_elem);
  return hash_bytes (&me->id, sizeof me->id);
}

static bool
mmap_entry_less (const struct hash_elem *a_, const struct hash_elem *b_,
           void *aux UNUSED)
{
  const struct mmap_entry *a = hash_entry (a_, struct mmap_entry, hash_elem);
  const struct mmap_entry *b = hash_entry (b_, struct mmap_entry, hash_elem);

  return a->id < b->id;
}

void
mmap_init (struct thread *t)
{
  hash_init (&t->mmap_entries, mmap_entry_hash, mmap_entry_less, NULL);
}

mapid_t
sys_mmap (int fd, void *addr)
{
  struct thread *t = thread_current ();

  bool present;
  lock_acquire (&t->pagedir_lock);
  present = pagedir_get_page (t->pagedir, addr) != NULL;
  lock_release (&t->pagedir_lock);

    if (addr == 0 || pg_ofs (addr) != 0 || present || page_present (addr))
      return -1;

    struct file *file;
    mapid_t id = -1;

    file = process_get_file (fd);
    if (file != NULL)
      {
        lock_acquire (&fslock);
        off_t len = file_length (file);
        lock_release (&fslock);
        off_t off = 0, bytes_left = len;
        uint8_t *upage = addr;
        size_t page_count = 0;

        while (bytes_left > 0) {
            size_t page_file_len = bytes_left > PGSIZE ? PGSIZE : bytes_left;

            page_add_file (upage, true, file, page_file_len, off);
            
            off += page_file_len;
            bytes_left -= page_file_len;
            upage += PGSIZE;
            page_count++;
        }

        if (off == len && len != 0)
          {
            id = hash_size (&t->mmap_entries);

            struct mmap_entry *me = malloc (sizeof (struct mmap_entry));
            me->id = id;
            me->upage = addr;
            me->page_count = page_count;
            
            hash_insert (&t->mmap_entries, &me->hash_elem);
          }        
      }

    return id;
}

void
sys_munmap (mapid_t id)
{
  struct thread *t = thread_current ();
  struct mmap_entry dummy_me = {.id = id};
  struct hash_elem *he = hash_find (&t->mmap_entries, &dummy_me.hash_elem);
  if (he != NULL)
  {
    struct mmap_entry *me = hash_entry (he, struct mmap_entry, hash_elem);
    page_remove (me->upage);
    hash_delete (&t->mmap_entries, &me->hash_elem);
    free (me);
  }
}

static  void hash_remove_mmap_entry (struct hash_elem *he, void *aux UNUSED)
{
  struct mmap_entry *me = hash_entry (he, struct mmap_entry, hash_elem);
  page_remove (me->upage);
  free (me);
}

void mmap_destory (void)
{
  hash_destroy (&thread_current ()->mmap_entries, hash_remove_mmap_entry);
}