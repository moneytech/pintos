#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

/* Very coarse lock to synchronize any access to filesystem code. */
struct lock fslock;

void syscall_init (void);

#endif /* userprog/syscall.h */
