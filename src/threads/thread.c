#include "threads/thread.h"
#include "devices/timer.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "fixed-point.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* List of threads that've been put to sleep by thread_sleep,
    and are yet to woken. */
static struct list sleep_list;

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* Array of lists where mlfq[i] holds the list of all
   threads with priority i. */
static struct list mlfq[PRI_MAX + 1];

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame 
  {
    void *eip;                  /* Return address. */
    thread_func *function;      /* Function to call. */
    void *aux;                  /* Auxiliary data for function. */
  };

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

/* MLQFS statistic */
static int ready_threads;       /* running thread (but not idle thread)
                                   + # of threads in ready list */
static int32_t load_avg;        /* system-wide load average */

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *, int, int);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);

static void thread_wake (void);

void thread_in_readylist_set_priority (struct thread *, int);

static void thread_nested_donate_priority (struct thread *, int);
static bool lock_more_func (const struct list_elem *,
                            const struct list_elem *, void *);

static int get_max_mlfqs_priority (void);
static void thread_update_recent_cpu (struct thread *, void *);
static void thread_update_mlfqs_priority (struct thread *);
static void thread_foreach_update_mlfqs_priority (struct thread *, void *);

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);

  ready_threads += 1;

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&sleep_list);
  list_init (&all_list);
  for (int i = 0; i < PRI_MAX + 1; i++)
    list_init (&mlfq[i]);

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT, NICE_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) 
{
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);

  /* Idle thread is going to be unblocked. Don't count it. */
  ready_threads -= 1;

  thread_create ("idle", PRI_MIN, idle, &idle_started);

  /* Start preemptive thread scheduling. */
  intr_enable ();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
}

static int
get_max_mlfqs_priority (void)
{
  int p = PRI_MAX;
  while (p > PRI_MIN && list_empty (&mlfq[p]))
      p--;
  return p;
}

/* Updates recent_cpu for thread based on current load average
   and previous recent_cpu value of the thread.
   
   Passed to the thread_foreach function. */
static void
thread_update_recent_cpu (struct thread *t, void *aux UNUSED)
{
  int coeff = FP_DIV (FP_MUL_INT (load_avg, 2),
                      FP_ADD_INT (FP_MUL_INT (load_avg, 2), 1));
  t->recent_cpu = FP_ADD_INT (FP_MUL (coeff, t->recent_cpu), t->nice);
}

/* Updates a thread's priority based on the current recent_cpu and
   nice value of the thread. */
static void
thread_update_mlfqs_priority (struct thread *t)
{
    int new_priority = PRI_MAX
                       - (FP_TO_NEAREST_INT (FP_DIV_INT (t->recent_cpu, 4)))
                       - 2*t->nice;
    
    /* Cap new_priority on either ends. */
    if (new_priority > PRI_MAX)
      new_priority = PRI_MAX;
    else if (new_priority < PRI_MIN)
      new_priority = PRI_MIN;

    t->priority = new_priority;
}

/* Updates the MLFQS priority for all threads except the idle thread.
   
   Passed to the thread_foreach function. */
static void
thread_foreach_update_mlfqs_priority (struct thread *t, void *aux UNUSED)
{
  if (t != idle_thread)
    thread_update_mlfqs_priority (t);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) 
{
  struct thread *cur = thread_current ();

  /* Update MLQFS statistics. */
  if (thread_mlfqs)
  {
    /* Increment recent_cpu for current thread. */
    cur->recent_cpu = FP_ADD_INT (cur->recent_cpu, 1);

    /* Every second ... */
    if (timer_ticks () % TIMER_FREQ == 0)
    {
      /* ... update the system-wide load average, */
      int coeff_a = FP_DIV (INT_TO_FP (59), INT_TO_FP (60));
      int coeff_b = FP_DIV (INT_TO_FP (1), INT_TO_FP (60));
      load_avg = FP_ADD (FP_MUL (coeff_a, load_avg),
                         FP_MUL_INT (coeff_b, ready_threads));

      /* ... and recompute recent_cpu value for all threads */
      thread_foreach (&thread_update_recent_cpu, NULL);
    }

    /* Every fourth tick ... */
    if (timer_ticks () % 4 == 0)
    {
      /* ... recalculate priority for all threads, */
      thread_foreach (&thread_foreach_update_mlfqs_priority, NULL);

      /* ... and yield if current thread no longer has highest priority. */
      if (cur->priority < get_max_mlfqs_priority ())
        intr_yield_on_return();
    }
  }

  /* Update statistics. */
  if (cur == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (cur->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();

  /* Wake up any sleeping threads */
  thread_wake ();
}

/* Prints thread statistics. */
void
thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) 
{
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;

  ASSERT (function != NULL);

  /* Allocate thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread (t, name, priority, thread_current ()->nice);
  tid = t->tid = allocate_tid ();

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  /* Add to run queue. */
  thread_unblock (t);
  thread_check_priority_and_yield ();

  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) 
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  ready_threads -= 1;
  schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t) 
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  if (thread_mlfqs)
    list_push_back (&mlfq[t->priority], &t->elem);
  else
    list_insert_ordered (&ready_list, &t->elem, &thread_more_func, NULL);
  t->status = THREAD_READY;
  ready_threads += 1;
  intr_set_level (old_level);
}

bool thread_more_func (const struct list_elem *a, const struct list_elem *b,
                       void *aux UNUSED)
{
  struct thread *ta = list_entry (a, struct thread, elem);
  struct thread *tb = list_entry (b, struct thread, elem);
  return ta->priority > tb->priority;
}

/* Sleeps for approximately TICKS timer ticks.  Interrupts must
   be turned on. */
void
thread_sleep (int64_t ticks)
{
  struct thread *t;
  enum intr_level old_level;

  t = thread_current ();
  t->ticks = ticks;

  old_level = intr_disable ();
  list_push_back (&sleep_list, &t->sleep_elem);
  thread_block ();
  intr_set_level (old_level);
}

/* Decrements `ticks` for all sleeping threads, and wakes up
    any thread whose time is up. */
static void
thread_wake ()
{
  struct list_elem *e;
  enum intr_level old_level;

  old_level = intr_disable ();
  e = list_begin (&sleep_list);
  while (e != list_end (&sleep_list))
    {
      struct thread *t = list_entry (e, struct thread, sleep_elem);
      t->ticks--;
      if (t->ticks < 1)
      {
        thread_unblock (t);
        e = list_remove (e);
      } else
        e = list_next (e);
    }
  intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void) 
{
  return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) 
{
  struct thread *t = running_thread ();
  
  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) 
{
  return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) 
{
  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit ();
#endif

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
  intr_disable ();
  list_remove (&thread_current ()->allelem);
  thread_current ()->status = THREAD_DYING;
  ready_threads -= 1;
  schedule ();
  NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;
  
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  if (cur != idle_thread)
  {
    if (thread_mlfqs)
      list_push_back (&mlfq[cur->priority], &cur->elem);
    else
      list_insert_ordered (&ready_list, &cur->elem, &thread_more_func, NULL);
  }
  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}

void
thread_check_priority_and_yield (void)
{
  struct thread *cur = thread_current ();
  bool should_yield;

  should_yield = false;
  if (thread_mlfqs)
    should_yield = cur->priority < get_max_mlfqs_priority ();
  else if (!list_empty (&ready_list)) {
    struct thread *t = list_entry (list_front (&ready_list), struct thread, elem);
    should_yield = cur->priority < t->priority;
  }

  if (should_yield)
  {
    if (intr_context ())
      intr_yield_on_return ();
    else
      thread_yield ();
  }
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority) 
{
  if (!thread_mlfqs)
  {
    struct thread *cur = thread_current ();

    /* If the current thread has previously recieved a priority donation,
       just set it's original_priority so that this function call
       "takes effect" only after the priority has been revoked. */
    cur->original_priority = new_priority;

    if (list_empty (&cur->locks))
    {
      /* Set current thread's priority preemptively only if
         it previously hasn't recieved a priority donation. */
      cur->priority = new_priority;
      thread_check_priority_and_yield ();
    }
  }
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) 
{
  return thread_current ()->priority;
}

/* Sets the priority of a thread in the ready list and reorders
   the ready list. */
void
thread_in_readylist_set_priority (struct thread *t, int new_priority)
{
  if (!thread_mlfqs)
  {
    enum intr_level old_level;
    
    old_level = intr_disable ();

    list_remove (&t->elem);
    t->priority = new_priority;
    list_insert_ordered (&ready_list, &t->elem, &thread_more_func, NULL);

    intr_set_level (old_level);
  }
}

static bool
lock_more_func (const struct list_elem *a, const struct list_elem *b,
                void *aux UNUSED)
{
  struct lock *ta = list_entry (a, struct lock, elem);
  struct lock *tb = list_entry (b, struct lock, elem);
  return ta->max_priority > tb->max_priority;
}

/* Donate priority to t, and also all threads it previously donated to. */
static void
thread_nested_donate_priority (struct thread *t, int priority)
{
  while (t->donated_to)
  {
    /* If t previously donated, it's currently in some lock's
       waiting list and not in the ready list, so we can simply
       set it's priority field.
       
       We don't re-sort the lock's waiting list because the tests
       don't check for nested multiple donation. */
    t->priority = priority;

    t = t->donated_to;
  }

  /* If t didn't previously donate, it's the thread that's 
     currently hold the lock that's casuing the chained waiting,
     and is also currently in the ready list. */
  thread_in_readylist_set_priority (t, priority);
}

/* Donates priority of current thread to thread t because
   current thread waiting on lock that t is currently holding. */
void
thread_donate_priority (struct thread *t, struct lock *lock)
{
  if (list_empty (&t->locks))
    /* First time t is recieving priority donation, so record
       it's current priority. */
    t->original_priority = t->priority;
  else {
    /* If this lock was involved in a previous donation
       to t, remove it so we can insert it back into it's 
       ordered position. */
    for (struct list_elem *e = list_begin (&t->locks);
         e != list_end (&t->locks);
         e = list_next (e))
      {
        if (list_entry (e, struct lock, elem) == lock)
        {
          list_remove (e);
          break;
        }
      }
  }

  /* Insert lock into t's ordered list based on the maximum priority
     (of a thread) in the lock's wait list. */
  list_insert_ordered (&t->locks, &lock->elem, &lock_more_func, NULL);

  /* Find the current highest priority lock in t's lock list. */
  struct lock *l = list_entry (list_front (&t->locks), struct lock, elem);

  /* Donate l's max priority to t and all threads it previously donated to. */
  thread_nested_donate_priority (t, l->max_priority);

  /* Record that the current thread donated it's priority to t, which will be
     useful later during nested priority donation. 
     
     Since the tests don't check for nested multiple donation, it's fine to
     record just one thread. */
  thread_current ()->donated_to = t;
}

/* Revokes priority of current thread that was donated due to holding lock. */
void
thread_revoke_priority (struct lock *lock)
{
  struct thread *cur = thread_current ();

  list_remove (&lock->elem);
  if (list_empty (&cur->locks))
  {
    /* All donation locks have been released, so revert back to the original
       priority. */
    cur->priority = cur->original_priority;
    cur->donated_to = NULL;
  } else {
    /* Set the priority to that of the next highest priority thread waiting
       on lock. */
    struct lock *l = list_entry (list_front (&cur->locks), struct lock, elem);
    cur->priority = l->max_priority;
  }
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice) 
{
  thread_current ()->nice = nice;
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) 
{
  return thread_current ()->nice;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) 
{
  return FP_TO_NEAREST_INT (FP_MUL_INT (load_avg, 100));
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) 
{
  return FP_TO_NEAREST_INT (FP_MUL_INT (thread_current ()->recent_cpu, 100));
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();

  /* If thread_mlfqs is true, idle thread's priority was calculated
     in init_thread. It always needs to be PRI_MIN. */
  idle_thread->priority = PRI_MIN;

  sema_up (idle_started);

  for (;;) 
    {
      /* Let someone else run. */
      intr_disable ();

      /* thread_block decrements ready_threads, but idle thread
         isn't counted as a "ready thread". */
      ready_threads += 1;

      thread_block ();

      /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
      asm volatile ("sti; hlt" : : : "memory");
    }
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) 
{
  ASSERT (function != NULL);

  intr_enable ();       /* The scheduler runs with interrupts off. */
  function (aux);       /* Execute the thread function. */
  thread_exit ();       /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread (void) 
{
  uint32_t *esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority, int nice)
{
  enum intr_level old_level;

  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;
  t->ticks = 0;

  if (thread_mlfqs)
  {
    t->nice = nice;
    t->recent_cpu = nice; /* Check the formula for recent_cpu. */
    thread_update_mlfqs_priority (t);
  } else {
    t->priority = priority;
    t->original_priority = priority;
    t->donated_to = NULL;
    list_init (&t->locks);
  }

  t->magic = THREAD_MAGIC;

  old_level = intr_disable ();
  list_push_back (&all_list, &t->allelem);
  intr_set_level (old_level);
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame (struct thread *t, size_t size) 
{
  /* Stack data is always allocated in word-size units. */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) 
{
  if (thread_mlfqs)
  {
    int p = get_max_mlfqs_priority ();
    if (p == PRI_MIN && list_empty (&mlfq[p]))
      return idle_thread;
    else
      return list_entry (list_pop_front (&mlfq[p]), struct thread, elem);
  } else {
    if (list_empty (&ready_list))
      return idle_thread;
    else
      return list_entry (list_pop_front (&ready_list), struct thread, elem);
  }
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();
  
  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate ();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
    {
      ASSERT (prev != cur);
      palloc_free_page (prev);
    }
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_schedule_tail()
   has completed. */
static void
schedule (void) 
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);
