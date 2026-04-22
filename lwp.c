#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#include "lwp.h"
#include "fp.h"

/* bc of mac */
#ifndef MAP_STACK
#define MAP_STACK 0
#endif

/* ---- Globals ---- */
/* who is running right now */
static thread current_thread = NULL;

/* counter for unique tids - 1 for call thrd*/
static tid_t next_tid = 1;

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
static int rr_count = 0;

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

static thread rr_next(void) {
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
    - howbig = ((rl.rlim_curr + page_size - 1) / page_size) * page_size
    - Round up to the nearest multiple of the page size.
      - page_size = long sysconf(_SC_PAGE_SIZE)
      - struct rlimit rl;
      - resource_limit = getrlimit(RLIMIT_STACK, &rl);
  
*/
tid_t lwp_create(lwpfun func, void *arg) {
  thread newThread = malloc(sizeof(*newThread));
  if (newThread == NULL) {
    return NO_THREAD;
  }

  /* get stack size from resource limit */
  size_t stack_size;
  struct rlimit rl;
  if (getrlimit(RLIMIT_STACK, &rl) == -1 || rl.rlim_cur == RLIM_INFINITY) {
    stack_size = 8 * 1024 * 1024;
  } else {
    stack_size = (size_t) rl.rlim_cur;
  }

  /* round up to page boundary */
  long page_size = sysconf(_SC_PAGE_SIZE);
  if (page_size == -1) {
    free(newThread);
    return NO_THREAD;
  }
  stack_size = ((stack_size + page_size - 1) / page_size) * page_size;

  /* allocate stack */
  void *thread_stack = mmap(NULL, stack_size,
      PROT_READ | PROT_WRITE,
      MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK,
      -1, 0);
  if (thread_stack == MAP_FAILED) {
    free(newThread);
    return NO_THREAD;
  }

  /* populate thread struct */
  newThread->tid = next_tid++;
  newThread->stacksize = stack_size;
  newThread->stack = (unsigned long *)thread_stack;
  newThread->status = LWP_LIVE;
  newThread->state.fxsave = FPU_INIT;
  newThread->lib_one = NULL;
  newThread->lib_two = NULL;
  newThread->sched_one = NULL;
  newThread->sched_two = NULL;
  newThread->exited = NULL;

  /* for sm garbage */
  memset(&newThread->state, 0, sizeof(rfile));
  newThread->state.fxsave = FPU_INIT;

  /* build the fake stack frame */
  unsigned long *sp = (unsigned long *)((char *)thread_stack + stack_size);
  sp = (unsigned long *)((unsigned long)sp & ~0xFUL);

  sp--;
  *sp = 0;
  sp--;
  *sp = (unsigned long) lwp_wrap;   /* return address */
  sp--;
  *sp = 0;                          /* fake old base pointer */

  newThread->state.rbp = (unsigned long) sp;
  newThread->state.rsp = (unsigned long) sp;
  newThread->state.rdi = (unsigned long) func;
  newThread->state.rsi = (unsigned long) arg;

  /* add to all_threads list */
  newThread->lib_one = all_threads;
  all_threads = newThread;

  /* init scheduler if needed */
  if (!current_scheduler){
    current_scheduler = RoundRobin;
  }
  current_scheduler->admit(newThread);
  return newThread->tid;
}



/*
void lwp start(void);
  - converts calling thead into a lwp
  - yields to another lwp
*/
void lwp_start(void) {
    thread newThread = malloc(sizeof(*newThread));
    if (newThread == NULL)
        return;

    newThread->tid = next_tid++;
    newThread->stack = NULL;        /* don't free this stack later */
    newThread->stacksize = 0;
    newThread->status = LWP_LIVE;
    newThread->lib_one = NULL;
    newThread->lib_two = NULL;
    newThread->sched_one = NULL;
    newThread->sched_two = NULL;
    newThread->exited = NULL;

    /* add to all_threads list */
    newThread->lib_one = all_threads;
    all_threads = newThread;

    /* init scheduler if needed */
    if (!current_scheduler){
      current_scheduler = RoundRobin;
    }
    current_scheduler->admit(newThread);

    current_thread = newThread;

    lwp_yield();
}

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
void lwp_exit(int status) {
  current_thread->status = MKTERMSTAT(LWP_TERM, status);
  current_scheduler->remove(current_thread);

  if (wait_head) {
    /* if someone wating - put them first */
    thread waiter = wait_head;
    wait_head = wait_head->lib_two;

    if (!wait_head) {
      wait_head = NULL;
    }
    waiter->lib_two = NULL;
    waiter->exited = current_thread;
    current_scheduler->admit(waiter);
  } 
  else {
    current_thread->lib_two = NULL;

    if (!term_head) {
      term_head = current_thread;
      term_tail = current_thread;
    } 
    else {
      term_tail->lib_two = current_thread;
      term_tail = current_thread;
    }
  }

  lwp_yield();
}

/*
tid t lwp wait(int *status);
  - return terminated threads in FIFO order
  - returns termination status or NO THREAD
*/

tid_t lwp_wait(int *status){
  thread t = term_head;

  if(t != NULL){
    /* Updated term head and tail as necessary */
    term_head = t -> exited;
    if (term_head == NULL) {
      term_tail = NULL;
    }
    /* Get info before freeing */
    tid_t termination_tid = t -> tid;
    int termination_stat = t -> status;

    /* If not the original system thread, unmap resources */
    if(t->stack != NULL){
      munmap(t->stack, t->stacksize);
    }
    /* Free the dynamically allocated context regardless */
    if(status != NULL){
      *status = termination_stat;
    }
    free(t);
    return termination_tid;
  }
  else{
    /* If there are no more threads that could possibly block */
    scheduler sched = lwp_get_scheduler();
    if(sched -> qlen() <= 1){
      return NO_THREAD;
    }
    else{
      sched -> remove(current_thread);
      current_thread -> exited = NULL;

      if (!wait_head) {
        wait_head = current_thread;
        wait_tail = current_thread;
      } 
      else {
        wait_tail -> exited = current_thread;
        wait_tail = current_thread;
      }
      lwp_yield();

      /*NEED TO MAKE SURE THAT YIELD AND WAIT INTERACT FINE HERElf*/
      t = current_thread -> exited;
      tid_t termination_tid = t->tid;
      
      if (status != NULL) {
        *status = t->status;
      }

      if (t->stack != NULL) {
        munmap(t->stack, t->stacksize);
      }

      free(t);
      return termination_tid;
    }
  }
}

/*
tid t lwp gettid(void);
  - returns id of calling lwp or NO THREAD
*/
tid_t lwp_gettid(void){
  if (current_thread){
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
 */
static void lwp_wrap(lwpfun fun, void *arg) {
  int rval = fun(arg);
  lwp_exit(rval);
}

/*
void lwp set scheduler(scheduler sched);
  - whatever scheduler is passed in will be new scheduler
    - just call next
  - if scheduler is null, do round robin
*/

void lwp_set_scheduler(scheduler sched) {
  if (sched == NULL)
    sched = RoundRobin;
  
  if (sched == current_scheduler){
    return;
  }
  scheduler old = current_scheduler;

  /* init new one first */
  current_scheduler = sched;
  if (current_scheduler->init){
    current_scheduler->init();
  }
  /* transfer one at a time: remove from old, admit to new */
  if (old) {
    thread t;
    while ((t = old->next()) != NULL) {
    old->remove(t);
    t->sched_one = NULL;
    t->sched_two = NULL;
    current_scheduler->admit(t);
  }
  /* shutdown old last */
    if (old->shutdown){
      old->shutdown();
    }
  }
  }

  /*
  scheduler lwp get scheduler(void);
    - Returns the pointer to the current scheduler.
  */
  scheduler lwp_get_scheduler(void) {
    return current_scheduler;
}
