#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#include "lwp.h"
#include "fp.h"

#ifndef MAP_STACK
#define MAP_STACK 0
#endif

#define DEFAULT_STACK_SIZE (8 * 1024 * 1024)

/* ---- Globals ---- */
static thread current_thread = NULL;
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

/* Forward declarations */
static void lwp_wrap(lwpfun fun, void *arg);



/* ROUND ROBIN SCHEDULER */

static thread rr_head = NULL;
static int rr_count = 0;

/*
 * rr_admit - Add a thread to the back of the round-robin queue.
 */
static void rr_admit(thread new) {
    if (!rr_head) {
        new->sched_one = new;
        new->sched_two = new;
        rr_head = new;
    }
    else {
        thread tail = rr_head->sched_two;
        new->sched_one = rr_head;
        new->sched_two = tail;
        tail->sched_one = new;
        rr_head->sched_two = new;
    }
    rr_count++;
}

/*
 * rr_remove - Remove a thread from the round-robin queue.
 */
static void rr_remove(thread victim) {
    if (rr_count == 1) {
        rr_head = NULL;
    }
    else {
        thread prev = victim->sched_two;
        thread next = victim->sched_one;
        prev->sched_one = next;
        next->sched_two = prev;
        if (rr_head == victim) {
            rr_head = next;
        }
    }
    victim->sched_one = NULL;
    victim->sched_two = NULL;
    rr_count--;
}

/*
 * rr_next - Return the next thread and rotate the queue.
 */
static thread rr_next(void) {
    if (!rr_head) {
        return NULL;
    }
    thread chosen = rr_head;
    rr_head = chosen->sched_one;
    return chosen;
}

/*
 * rr_qlen - Return the number of threads in the queue.
 */
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


/* HELPER: remove from all_threads */
/* Removes a thread from the global all_threads list 
    so that tid2thread will not walk 
    into freed memory. */

static void remove_from_all(thread dead) {
    if (all_threads == dead) {
        all_threads = dead->lib_one;
    }
    else {
        thread cur = all_threads;
        while (cur && cur->lib_one != dead) {
            cur = cur->lib_one;
        }
        if (cur) {
            cur->lib_one = dead->lib_one;
        }
    }
}


/*
 * lwp_wrap - Wrapper that calls the thread function and then
 *            passes its return value to lwp_exit.
 */
static void lwp_wrap(lwpfun fun, void *arg) {
    int rval;
    rval = fun(arg);
    lwp_exit(rval);
}

/*
 * lwp_create - Create a new lightweight process that will execute
 *              the given function with the given argument.
 *              Returns the thread id or NO_THREAD on failure.
 */
tid_t lwp_create(lwpfun func, void *arg) {
    size_t stack_size;
    struct rlimit rl;
    long page_size;
    void *thread_stack;
    unsigned long *sp;

    thread newThread = malloc(sizeof(*newThread));
    if (newThread == NULL) {
        return NO_THREAD;
    }

    if (getrlimit(RLIMIT_STACK, &rl) == -1 ||
        rl.rlim_cur == RLIM_INFINITY) {
        stack_size = DEFAULT_STACK_SIZE;
    }
    else {
        stack_size = (size_t)rl.rlim_cur;
    }

    /* Round up to nearest page boundary */
    page_size = sysconf(_SC_PAGE_SIZE);
    if (page_size == -1) {
        free(newThread);
        return NO_THREAD;
    }
    stack_size = ((stack_size + page_size - 1) / page_size) * page_size;

    /* Allocate stack via mmap */
    thread_stack = mmap(NULL, stack_size,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK,
        -1, 0);
    if (thread_stack == MAP_FAILED) {
        free(newThread);
        return NO_THREAD;
    }

    /* Populate the thread context */
    newThread->tid = next_tid++;
    newThread->stacksize = stack_size;
    newThread->stack = (unsigned long *)thread_stack;
    newThread->status = LWP_LIVE;
    newThread->lib_one = NULL;
    newThread->lib_two = NULL;
    newThread->sched_one = NULL;
    newThread->sched_two = NULL;
    newThread->exited = NULL;

    /* Zero all registers and initialize FPU state */
    memset(&newThread->state, 0, sizeof(rfile));
    newThread->state.fxsave = FPU_INIT;

    /* Build the fake stack frame for swap_rfiles to tear down. */
    sp = (unsigned long *)((char *)thread_stack + stack_size);
    sp = (unsigned long *)((unsigned long)sp & ~0xFUL);

    sp--;
    *sp = 0;                             /* alignment padding */
    sp--;
    *sp = (unsigned long)lwp_wrap;       /* return address */
    sp--;
    *sp = 0;                             /* fake old base pointer */

    newThread->state.rbp = (unsigned long)sp;
    newThread->state.rsp = (unsigned long)sp;
    newThread->state.rdi = (unsigned long)func;
    newThread->state.rsi = (unsigned long)arg;

    newThread->lib_one = all_threads;
    all_threads = newThread;

    if (!current_scheduler) {
        current_scheduler = RoundRobin;
    }
    current_scheduler->admit(newThread);

    return newThread->tid;
}

/*
 * lwp_start - Convert the calling thread (the original system thread)
 *             into a LWP and yield to the first scheduled thread.
 */
void lwp_start(void) {
    thread newThread = malloc(sizeof(*newThread));
    if (newThread == NULL) {
        return;
    }

    newThread->tid = next_tid++;
    newThread->stack = NULL;
    newThread->stacksize = 0;
    newThread->status = LWP_LIVE;
    newThread->lib_one = NULL;
    newThread->lib_two = NULL;
    newThread->sched_one = NULL;
    newThread->sched_two = NULL;
    newThread->exited = NULL;

    newThread->lib_one = all_threads;
    all_threads = newThread;

    if (!current_scheduler) {
        current_scheduler = RoundRobin;
    }
    current_scheduler->admit(newThread);

    current_thread = newThread;

    lwp_yield();
}

/*
 * lwp_yield - Save the current thread's context, pick the next
 *             thread from the scheduler, and switch to it.
 *             If no runnable threads remain, exit the program.
 */
void lwp_yield(void) {
    thread prev;
    thread next = current_scheduler->next();

    if (!next) {
        exit(LWPTERMSTAT(current_thread->status));
    }

    prev = current_thread;
    current_thread = next;
    swap_rfiles(&prev->state, &next->state);
}

/*
 * lwp_exit - Terminate the calling thread with the given status.
 *            If a thread is blocked in lwp_wait, hand the dead
 *            thread directly to it.  Otherwise queue it for later
 *            collection.  Then yield to the next thread.
 */
void lwp_exit(int status) {
    current_thread->status = MKTERMSTAT(LWP_TERM, status);
    current_scheduler->remove(current_thread);

    if (wait_head) {
        /* A thread is blocked in lwp_wait — hand off directly */
        thread waiter = wait_head;
        wait_head = wait_head->lib_two;
        if (!wait_head) {
            wait_tail = NULL;
        }
        waiter->lib_two = NULL;
        waiter->exited = current_thread;
        current_scheduler->admit(waiter);
    }
    else {
        /* No waiters — add to terminated queue */
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
 * lwp_wait - Wait for a thread to terminate, deallocate its
 *            resources, and report its termination status.
 *            Returns the tid of the terminated thread or
 *            NO_THREAD if waiting would block forever.
 */
tid_t lwp_wait(int *status) {
    thread dead;
    tid_t tid;

    /* If there are already terminated threads, grab the oldest */
    if (term_head) {
        dead = term_head;
        term_head = dead->lib_two;
        if (!term_head) {
            term_tail = NULL;
        }
        dead->lib_two = NULL;

        tid = dead->tid;
        if (status) {
            *status = dead->status;
        }

        /* Free stack (but not the original system thread's stack) */
        if (dead->stack) {
            munmap(dead->stack, dead->stacksize);
        }

        remove_from_all(dead);
        free(dead);
        return tid;
    }

    /* No terminated threads — would we block forever? */
    if (current_scheduler->qlen() <= 1) {
        return NO_THREAD;
    }

    /* Block: remove self from scheduler and join wait queue */
    current_scheduler->remove(current_thread);
    current_thread->lib_two = NULL;
    if (!wait_head) {
        wait_head = current_thread;
        wait_tail = current_thread;
    }
    else {
        wait_tail->lib_two = current_thread;
        wait_tail = current_thread;
    }

    lwp_yield();

    /* Woken up by lwp_exit — exited pointer tells us who died */
    dead = current_thread->exited;
    current_thread->exited = NULL;

    tid = dead->tid;
    if (status) {
        *status = dead->status;
    }

    if (dead->stack) {
        munmap(dead->stack, dead->stacksize);
    }

    remove_from_all(dead);
    free(dead);
    return tid;
}

/*
 * lwp_gettid - Return the tid of the calling LWP,
 *              or NO_THREAD if not called by a LWP.
 */
tid_t lwp_gettid(void) {
    if (current_thread) {
        return current_thread->tid;
    }
    return NO_THREAD;
}

/*
 * tid2thread - Return the thread context for the given tid,
 *              or NULL if the tid is invalid.
 */
thread tid2thread(tid_t tid) {
    thread t = all_threads;

    while (t) {
        if (t->tid == tid) {
            return t;
        }
        t = t->lib_one;
    }
    return NULL;
}

/*
 * lwp_set_scheduler - Install a new scheduler.  Transfer all threads
 *                     from the old scheduler to the new one.
 *                     If sched is NULL, revert to round-robin.
 */
void lwp_set_scheduler(scheduler sched) {
    scheduler old;

    if (sched == NULL) {
        sched = RoundRobin;
    }

    old = current_scheduler;

    /* Initialize the new scheduler FIRST!! */
    current_scheduler = sched;
    if (current_scheduler->init) {
        current_scheduler->init();
    }

    /* Transfer threads one at a time from old to new */
    if (old) {
        thread t;
        while ((t = old->next()) != NULL) {
            old->remove(t);
            t->sched_one = NULL;
            t->sched_two = NULL;
            current_scheduler->admit(t);
        }
        if (old->shutdown) {
            old->shutdown();
        }
    }
}

/*
 * lwp_get_scheduler - Return a pointer to the current scheduler.
 */
scheduler lwp_get_scheduler(void) {
    return current_scheduler;
}
