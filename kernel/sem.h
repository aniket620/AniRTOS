/*
 * A counting semaphore - the first synchronization primitive in the
 * project, and the one everything else in Stage 5+ (mutexes, message
 * queues) will end up built from or modeled after. See
 * docs/blocking_and_semaphores.md for the full design writeup; this
 * header is deliberately just the API and the struct layout.
 */
#ifndef SEM_H
#define SEM_H

#include "kernel.h"

typedef struct {
    /* How many "units" are available to be taken without blocking.
     * `volatile` because it's read and written from both ordinary task
     * code and (briefly, inside a critical section) whatever context
     * calls sem_post()/sem_wait() - the compiler must not assume it's
     * stable across those calls. */
    volatile int32_t count;

    /* Head of an intrusive singly-linked list of TCBs currently blocked
     * in sem_wait() on THIS semaphore, threaded through each task's own
     * `next_waiter` field (kernel/kernel.h). NULL when nobody's waiting.
     *
     * Invariant worth stating explicitly: `count` and `waiters` are
     * never both "active" at once in a correct implementation - either
     * there are unclaimed units sitting in `count` (nobody's waiting for
     * them yet), or there are tasks queued in `waiters` (there's nothing
     * left in `count` for them to have taken). See sem_post() for
     * exactly how that invariant is maintained. */
    TCB_t *waiters;
} sem_t;

/* Set up a semaphore with `initial_count` units immediately available
 * (0 is common - "nothing's happened yet, the first sem_wait() should
 * block until someone posts"). Call once, before any task calls
 * sem_wait()/sem_post() on it. */
void sem_init(sem_t *sem, int32_t initial_count);

/* Take one unit. If one's available (count > 0), returns immediately -
 * no blocking, no scheduler involvement, just a decrement. If none is
 * available, the calling task is marked TASK_BLOCKED, queued on this
 * semaphore's waiter list, and control is handed to the scheduler
 * (scheduler_yield()) - this call does not return until some later
 * sem_post() (from another task) wakes this one back up and the
 * scheduler gets around to running it again. */
void sem_wait(sem_t *sem);

/* Give back one unit. If a task is already waiting, it's woken directly
 * (handed the unit, without ever touching `count` - see the source for
 * why) and, since Stage 5b, control passes to the scheduler right away
 * so a higher-priority task that was just woken runs IMMEDIATELY rather
 * than waiting for the next SysTick tick - that immediacy is what
 * "priority-based preemption" actually means. If nobody's waiting, the
 * unit is simply banked in `count` for whichever task calls sem_wait()
 * next, and this never blocks either way. See kernel/sem.c and
 * docs/priority_scheduling.md for the full reasoning. */
void sem_post(sem_t *sem);

#endif /* SEM_H */
