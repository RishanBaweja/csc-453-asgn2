/*
Overall Assignment Goal:
  - Implement support for threads
  - Threads execute in the same address space as other threads
  - We are implementing a user-level thread package
  - This will be done by taking the orignal system thread that runs main and having it 
  create an arbitrary # of threads
  - Generally:
      - lwp_create will create a thread, but it will not run anything
        until it is returned to by lwp_yield
      - lwp_yield yields control of current thread to another thread
      - lwp_exit terminates curr thread and switches to another, if any
*/

/*
struct threadinfo_st{
  int thread_id;
  void *mmap_pointer;
  int status;
}
*/

/*
tid t lwp create(lwpfun function, void *argument);
  - Executes a function with arguments 
  - Returns tid_t thread id or NO THREAD
  - NO THREAD is in library
  - FOR FPU DOES THAT COME FROM CREATE?
    - library value
  - thread’s resources will consist of a context and stack, both initialized
  - s = mmap(NULL,howbig,PROT READ|PROT WRITE,MAP PRIVATE|MAP ANONYMOUS|MAP STACK,-1,0);
    - howbig = ((rl.rlim_curr + page_size - 1) / page_size) * page_size
    - Round up to the nearest multiple of the page size.
      - page_size = long sysconf(_SC_PAGE_SIZE)
      - struct rlimit rl;
      - resource_limit = getrlimit(RLIMIT_STACK, &rl);
  
*/

/*
void lwp start(void);
  - converts calling thead into a lwp
  - yields to another lwp
*/

/* 
void lwp yield(void);
  - Yields curr thread to another thread based on the scheduler
  - Saves curr thread information and transfers it to the other thread
  - swap rfiles(rfile *old, rfile *new)
  - If no thread, exit
*/

/*
void lwp exit(int exitval);
  - terminates curr thread
  - yields to whichever thread the scheduler chooses
  - DOES THAT JUST MEAN IT CALLS YIELD
    - Yes
  - if no other thread, exit
*/

/*
tid t lwp wait(int *status);
  - waits for a thread to finish and deallocates resources, if no available thread, but still runnable threads, block until there is one
  - return terminated threads in FIFO order
  - returns termination status or NO THREAD
*/

/*
tid t lwp gettid(void);
  - returns id of calling lwp or NO THREAD
*/
/*
thread tid2thread(tid t tid);
  - returns the thread or NO THREAD
*/

/*
void lwp set scheduler(scheduler sched);
  - whatever scheduler is passed in will be new scheduler
  - old scheduler should transfer its threads to the new scheduler in next() order
    - just call next
  - if scheduler is null, do round robin
*/
/*
scheduler lwp get scheduler(void);
  - Returns the pointer to the current scheduler.
*/

/*
  - #define LWPTERMSTAT(s) extracts the exit code from a status
  - how does it extract the exit code

*/