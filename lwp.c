#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#include "lwp.h"

/* ---- Globals ---- */
static thread current_thread = NULL;   /* who is running right now */
static tid_t next_tid = 1;            /* counter for unique tids */

/* linked list of ALL threads(for tid2thread) */
static thread all_threads = NULL;

/* terminated queue (FIFO) */
static thread term_head = NULL;
static thread term_tail = NULL;

/* waiting queue (threads blocked in lwp_wait) */
static thread wait_head = NULL;
static thread wait_tail = NULL;

/* the active scheduler */
static scheduler current_scheduler = NULL;

/* forward declarations */
static void lwp_wrap(lwpfun fun, void *arg);

/* ROUND ROBIN */
static thread rr_head = NULL;
static thread rr_count = 0;

static void rr_admit(thread new) {
  if (!rr_head) {
    /* no other threads = pt to self */
    new->sched_one = new;
    new->sched_two = new;
    rr_head = new;
  } else {
    /* insert before head (FIFO) */
    thread tail = rr_head->sched_two;
    new->sched_one = rr_head;
    new->sched_two = tail;
    tail->sched_one = new;
    rr_head->sched_two = new;
  }

  rr_count++;
}

static void rr_remove(thread victim) {
  if (rr_count == 1) {
    rr_head = NULL;
  } else {
    thread prev = victim->sched_two;
    thread next = victim->sched_one;
    prev->sched_one = next;
    next->sched_two = prev;
    if (rr_head == victim){ /* update new head case */
      rr_head = next;
    }
  }
  victim->sched_one = NULL;
  victim->sched_two = NULL;
  rr_count--;
}

static void rr_next(void) {
  if (!rr_head){
    return NULL;
  }

  thread chosen = rr_head;
  rr_head = chosen->sched_one; /* skssisisisi */
  return chosen;
}

static int rr_qlen(void) {
    return rr_count;
}

static struct scheduler rr_publish = {
  NULL, NULL, rr_admit, rr_remove, rr_next, rr_qlen
};
scheduler RoundRobin = &rr_publish;


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
void lwp_yield(void) {
  thread next = current_scheduler->next();

  if (!next) {
    exit(LWPTERMSTAT(current_thread->status));
  }

  thread prev = current_thread;
  current_thread = next;
  swap_rfiles(&prev->state, &next->state);
}

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
tid_t lwp_gettid(void) {
  if (current_thread) {
    return current_thread->tid;
  }
  return NO_THREAD;
}

/*
thread tid2thread(tid t tid);
  - returns the thread or NO THREAD
*/
thread tid2thread(tid_t tid) {
  thread t = all_threads;

  while (t) {
    if (t->tid == tid){
      return t;
    }
    t = t->lib_one;
  }
  return NULL; /* if found none */
}

/* lwp_wrap(void);
  - instead of returning %rax and exit looking at %rdi, we wrap it to make it simpler
 */
static void lwp_wrap(lwpfun fun, void *arg) {
    int rval = fun(arg);
    lwp_exit(rval);
}

/*
void lwp set scheduler(scheduler sched);
  - whatever scheduler is passed in will be new scheduler
  - old scheduler should transfer its threads to the new scheduler in next() order
    - just call next
  - if scheduler is null, do round robin
*/
void lwp_set_scheduler(scheduler sched) {
  if (sched = NULL) {
    sched = RoundRobin;
  }

  /* get threads from old scheduler */
  thread threads = NULL;
  thread tail = NULL;

  if (current_scheduler) {
    thread t;
    while ((t = current_scheduler->next()) != NULL) {
      current_scheduler->remove(t);

      t->sched_one = NULL;
      if (!threads) {
        threads = t;
        tail = t;
      } else {
        tail->sched_one = t;
        tail = t;
      }
    }
    if (current_scheduler->shutdown){
      current_scheduler->shutdown();
    }
  }

  current_scheduler = sched;

  /* initialize if needed */
  if (current_scheduler->init){
    current_scheduler->init();
  }

  /* add all collected threads */
  while (threads) {
    thread next = threads->sched_one;
    threads->sched_one = NULL;
    current_scheduler->admit(threads);
    threads = next;
  }
}

/*
scheduler lwp get scheduler(void);
  - Returns the pointer to the current scheduler.
*/
scheduler lwp_get_scheduler(void) {
  return current_scheduler;
}

/*
  - #define LWPTERMSTAT(s) extracts the exit code from a status
  - how does it extract the exit code

*/