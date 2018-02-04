# Pintos

This repository contains the Pintos operating system with features I've implemented which are described
in Stanford's [CS140 course](https://web.stanford.edu/class/cs140/projects/pintos/pintos.html).

They include:

* Thread scheduling (in branch [master](https://github.com/saurvs/pintos/tree/master))
  * Non-busy-wait sleeping
  * Priority round-robin scheduling
  * Priority donation with mutexes
  * Multilevel feedback queue scheduler similar to the 4.4BSD scheduler
  
* User programs (in branch [master](https://github.com/saurvs/pintos/tree/master))
  * POSIX-like process management system calls like `exec` and `wait` 
  * Common POSIX-like file I/O systems calls
  * Argument passing to `main` function
  * Denying writes to executable files of processes
  
* Virtual memory (in branch [project3](https://github.com/saurvs/pintos/tree/project3))
  * Demand paging
  * Swapping pages to disk blocks
  * Memory-mapped files
  * User process stack-growth
