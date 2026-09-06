/*
 * Counting semaphore implementation. See kernel/sem.h for the API and
 * docs/blocking_and_semaphores.md for the full reasoning - this file's
 * comments cover just the mechanics of these specific functions.
 */
#include "sem.h"

void sem_init(sem_t *sem, int32_t initial_count)
{
    sem->count = initial_count;
    sem->waiters = 0;
}

/*
 * Both sem_wait() and sem_post() wrap their real work in a PRIMASK-based
 * critical section: `cpsid i` (mask every exception with a configurable
 * priority - which is everything except NMI and HardFault) on the way
 * in, `cpsie i` on the way out. This is the bluntest possible tool for
 * the job - it doesn't just block a context switch, it blocks EVERY
 * interrupt in the system, including ones that have nothing to do with
 * scheduling, for the few instructions in between. That's deliberate for
 * now: `count`, `waiters`, and a task's `state`/`next_waiter` fields are
 * all touched together here and have to change as one atomic unit, or a
 * SysTick tick (or, later, some other interrupt) landing mid-update could
 * observe or create an inconsistent state - a task marked BLOCKED but
 * never actually linked into `waiters`, for instance, which would leave
 * it lost forever with nothing able to wake it. PRIMASK guarantees
 * nothing can interrupt that update. The roadmap's Stage 7 replaces this
 * blunt "stop the whole world" approach with BASEPRI-based critical
 * sections that only block interrupts at or below the kernel's own
 * priority - a real refinement, but not a correctness requirement yet,
 * since nothing above the kernel's priority exists in this project so
 * far.
 */

void sem_wait(sem_t *sem)
{
    __asm__ volatile ("cpsid i");

    if (sem->count > 0) {
        /* Fast path: a unit was already banked (or exactly balances an
         * earlier sem_post() that ran before we ever got here) - take it
         * and go, no blocking, no scheduler involvement at all. */
        sem->count--;
        __asm__ volatile ("cpsie i");
        return;
    }

    /* Slow path: nothing available. Queue ourselves and give up the
     * CPU. Pushing onto the FRONT of the list (not the back) is a
     * deliberate simplification: this is a stack, not a FIFO queue, so
     * wakeup order among multiple waiters is LIFO, not first-come-
     * first-served. That's a genuine fairness wrinkle - fine for this
     * project's demo (which never has more than one waiter on a given
     * semaphore at a time), but worth knowing if you ever have several
     * tasks queued on the same semaphore and care which one wakes up
     * first. A real FIFO would need a tail pointer too; not worth the
     * extra bookkeeping yet. */
    current_task->state = TASK_BLOCKED;
    current_task->next_waiter = sem->waiters;
    sem->waiters = current_task;

    __asm__ volatile ("cpsie i");

    /* Ask the scheduler for someone else. This call does not return to
     * this exact point until some later sem_post() finds this task on
     * `waiters`, marks it TASK_READY again, and the round-robin
     * scheduler eventually picks it back up - which, from this task's
     * own point of view, looks exactly like an ordinary function call
     * that just happened to take an arbitrarily long time. Same
     * "returns later" property Stage 3's kernel_switch_to() had -
     * see docs/context_switch.md. */
    scheduler_yield();
}

void sem_post(sem_t *sem)
{
    TCB_t *woken = 0;

    __asm__ volatile ("cpsid i");

    if (sem->waiters != 0) {
        /* Someone's already queued - hand the unit DIRECTLY to them
         * rather than incrementing `count` and letting them go re-claim
         * it. This is what keeps the "count and waiters are never both
         * active" invariant true: if we incremented count here too, an
         * unrelated THIRD task calling sem_wait() before the woken task
         * gets scheduled could steal the unit meant for it - correct
         * counting-semaphore behavior would be broken. Direct hand-off
         * means the unit is already spoken for the instant sem_post()
         * runs, not merely "available". */
        woken = sem->waiters;
        sem->waiters = woken->next_waiter;
        woken->next_waiter = 0;
        woken->state = TASK_READY;
    } else {
        /* Nobody's waiting - bank the unit for whoever calls sem_wait()
         * next, whenever that is. */
        sem->count++;
    }

    __asm__ volatile ("cpsie i");

    /*
     * Stage 5a said sem_post() never forces a switch - marking a task
     * TASK_READY only changed its ELIGIBILITY, and the round-robin
     * scheduler alone decided who actually ran, whenever it next felt
     * like deciding. Stage 5b changes this, and deliberately: THIS is
     * what "priority-based preemption" means in practice. If `woken`
     * genuinely outranks whoever's running right now, that has to take
     * effect immediately - not whenever the next SysTick tick happens to
     * land (up to 20ms later, per TIME_SLICE_TICKS). A task waiting on a
     * high-priority event and getting to it "eventually, next tick"
     * defeats the entire point of giving it a high priority.
     *
     * Calling scheduler_yield() unconditionally here (only when we
     * actually woke someone - never on the plain "bank a unit" path,
     * where nothing's eligibility changed) is still safe and cheap even
     * when it turns out NOT to be warranted: scheduler_yield() always
     * re-derives "who's actually highest-priority-ready" via
     * pick_next_ready() itself, and its self-check already handles "the
     * current task is still the best choice" as a no-op (see
     * kernel/scheduler.c). This function doesn't need to compare
     * priorities itself - it just asks the one place that already knows
     * how to answer that question, every time something might have
     * changed.
     *
     * One honest side effect worth naming: if `woken` turns out to be
     * the SAME priority as whoever's running (not higher), this can
     * still cause an immediate switch to it, if it's next up in the
     * round-robin order - slightly different from strict textbook/
     * FreeRTOS-style semantics, where only a STRICTLY higher priority
     * wake preempts immediately, and an equal-priority wake just joins
     * the ready line for its normal turn. Harmless here (it's still a
     * legitimate round-robin turn, just taken a little early), and not
     * worth the extra bookkeeping to special-case yet.
     */
    if (woken != 0) {
        scheduler_yield();
    }
}
