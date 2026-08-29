/*
 * Stage 4: the tick and the policy layer on top of Stage 3's mechanism.
 *
 * Stage 3 gave us a mechanism - "switch to whichever task next_task
 * points at, whenever something pends PendSV" - but nothing decided
 * *when* or *to whom*. Tasks decided that themselves, by calling
 * kernel_switch_to() voluntarily. This file replaces that voluntary
 * handoff with a timer-driven policy: SysTick fires on a fixed schedule,
 * and once every task has had its fair turn, THIS file decides who runs
 * next - the tasks themselves no longer have any say in it. That's the
 * real difference between "cooperative" (Stage 3) and "preemptive"
 * (Stage 4) multitasking.
 *
 * ============================================================
 * Why SysTick_Handler must not perform the switch itself
 * ============================================================
 * It would be technically *possible* to write SysTick_Handler so it
 * directly does what PendSV_Handler does - save R4-R11, load the next
 * task's saved R4-R11, flip PSP. Nothing stops you from writing that
 * code. But it would be a real, dangerous bug, for a reason that only
 * shows up once *other* interrupts exist in the system:
 *
 *   SysTick is just one interrupt among many (UART RX, a button EXTI
 *   line, a timer capture - whatever a later stage adds). Suppose one of
 *   those fires, and *while it's still running*, SysTick also fires and
 *   preempts it (SysTick can be given a higher priority than ordinary
 *   peripheral interrupts - and often should be, so the tick stays
 *   accurate). If SysTick_Handler switched stacks right there, the
 *   *nested* interrupt's own return address and state - sitting on the
 *   outgoing task's stack, mid-way through being unwound - would get
 *   corrupted or lost. Exception return only unwinds correctly when
 *   nesting is respected: whichever exception handler is innermost must
 *   be the one that returns first, onto the exact stack it interrupted.
 *   Swapping PSP out from under a nested interrupt breaks that
 *   invariant.
 *
 * PendSV exists specifically to be the *only* handler that ever touches
 * PSP, and it's given the LOWEST priority in the entire system (see
 * kernel_init(), kernel/task.c) so that it can only ever run once
 * EVERYTHING else - every real interrupt, and SysTick itself - has
 * finished and there's nothing left to nest inside of. So the rule this
 * file follows is: SysTick_Handler is allowed to *decide* who runs next
 * and *ask* for a switch (by setting next_task and writing ICSR, via
 * kernel_switch_to() - exactly the same call a task made voluntarily in
 * Stage 3), but the actual stack-swapping act is deferred to PendSV,
 * which by the time it runs, is guaranteed nothing else is mid-flight.
 * "SysTick decides, PendSV acts" - that's the whole design in five words.
 *
 * ============================================================
 * Mechanism vs policy - concretely, in this codebase
 * ============================================================
 * kernel/task.c + kernel/pendsv.S:  MECHANISM. "Given a next_task
 *   pointer, correctly move the CPU onto it." Doesn't know or care how
 *   next_task got decided - could be a human calling kernel_switch_to()
 *   by hand (Stage 3), or this file's round-robin logic, or a future
 *   priority-based scheduler in a later stage. The mechanism doesn't
 *   change no matter which policy sits above it.
 * kernel/scheduler.c (this file):  POLICY. "Given several tasks that all
 *   want to run, decide who runs next, and when." Everything in here
 *   could be ripped out and replaced (priority scheduling, earliest-
 *   deadline-first, whatever) without touching task.c or pendsv.S at
 *   all - which is exactly the point of separating them.
 */
#include "kernel.h"
#include "system_stm32f4.h"

/* ============================================================
 * SysTick - a core peripheral (part of the ARMv7-M architecture itself,
 * not the STM32 chip), so its registers live in a fixed address range
 * outside any vendor's peripheral map, same reasoning as task.c's SCB
 * registers.
 * ============================================================ */
#define SYST_CSR   (*(volatile uint32_t *)0xE000E010UL)  /* control/status */
#define SYST_RVR   (*(volatile uint32_t *)0xE000E014UL)  /* reload value */
#define SYST_CVR   (*(volatile uint32_t *)0xE000E018UL)  /* current value */

#define SYST_CSR_ENABLE     (1UL << 0)  /* start the counter */
#define SYST_CSR_TICKINT    (1UL << 1)  /* fire SysTick_Handler when it hits 0 */
#define SYST_CSR_CLKSOURCE  (1UL << 2)  /* 1 = processor clock (not an /8 external ref) */

/* 1000 ticks/second = a 1ms tick - fine-grained enough to be useful, and
 * a round number for the arithmetic below. TIME_SLICE_TICKS=20 means
 * each task gets a 20ms slice before being preempted for the next one -
 * short enough to look "simultaneous" to a human watching an LED, long
 * enough that switching overhead (a few dozen instructions in PendSV) is
 * a rounding error by comparison. Both are just numbers to tune, not
 * load-bearing constants - see docs/tick_scheduler.md for the math. */
#define TICK_HZ            1000UL
#define TIME_SLICE_TICKS   20UL

/* Round-robin ready list. Fixed-size array, not a linked list - this is
 * a learning kernel and a linked list buys nothing yet (no removal, no
 * dynamic creation). MAX_TASKS is a ceiling, not a target; Stage 4's
 * demo only ever adds 2. */
#define MAX_TASKS 8

/* Renamed from Stage 4's `ready_list`: this array now holds EVERY task
 * that's ever been registered, whether it's currently eligible to run or
 * not - "ready" stopped being an accurate name for it the moment
 * TASK_BLOCKED became possible. Each entry's own `state` field is what
 * actually says whether it's a candidate right now; see
 * pick_next_ready(). */
static TCB_t *task_list[MAX_TASKS];
static uint32_t num_tasks = 0;
static uint32_t current_index = 0;
static volatile uint32_t slice_remaining = TIME_SLICE_TICKS;

void scheduler_add_task(TCB_t *tcb)
{
    if (num_tasks >= MAX_TASKS) {
        /* Same defensive spirit as task.c's task_exit_trap(): fail
         * loudly and stop, rather than silently dropping a task and
         * leaving you to wonder why it never runs. */
        for (;;) {
        }
    }
    task_list[num_tasks] = tcb;
    num_tasks++;
}

/*
 * The one place "who runs next" is actually decided. Walks task_list in
 * round-robin order STARTING RIGHT AFTER the current task, looking for
 * the first one that's TASK_READY - skipping any TASK_BLOCKED task
 * entirely, which is the entire mechanism by which blocking removes a
 * task from the rotation without needing a second data structure to
 * track "who's still eligible".
 *
 * The search deliberately walks all the way around, including back to
 * the CURRENT task's own slot (i == num_tasks checks index
 * current_index itself). Two outcomes fall out of that for free, with no
 * special-casing needed:
 *
 *   - If some OTHER task is READY, it's found first (it's earlier in the
 *     search order) and returned - normal round-robin.
 *   - If NO other task is READY: if the current task is itself still
 *     READY (the ordinary Stage-4-style case - nothing blocked, just a
 *     single-task or fully-busy system), the search eventually comes
 *     back around to it and returns ITS OWN index - correctly signaling
 *     "nobody else to switch to". If the current task is BLOCKED too
 *     (it just called sem_wait() and lost on the fast path), that same
 *     final check fails as well, and -1 comes back: genuinely nothing in
 *     the whole system is runnable.
 *
 * That last case is a real, currently-unhandled limitation, not a
 * far-fetched edge case: this project has no idle task yet (that's
 * Stage 10's job - see the roadmap). Until then, whatever's built on top
 * of this scheduler has to guarantee at least one task is always
 * TASK_READY. Stage 5's demo (app/main.c) is deliberately designed
 * around that constraint - see docs/blocking_and_semaphores.md.
 */
static int32_t pick_next_ready(void)
{
    for (uint32_t i = 1; i <= num_tasks; i++) {
        uint32_t idx = (current_index + i) % num_tasks;
        if (task_list[idx]->state == TASK_READY) {
            return (int32_t)idx;
        }
    }
    return -1;   /* nothing runnable anywhere - see the comment above */
}

void scheduler_yield(void)
{
    int32_t next = pick_next_ready();

    if (next < 0) {
        /* Nothing READY anywhere, including whoever called this. No
         * idle task exists to fall back to yet (Stage 10). Trap loudly
         * rather than silently returning into a task that has no
         * business running - see docs/blocking_and_semaphores.md. */
        for (;;) {
        }
    }

    if ((uint32_t)next == current_index) {
        /* pick_next_ready() only returns the current task's own index
         * when nothing ELSE is READY - meaning the caller is still
         * READY itself (a blocked caller can never match its own index;
         * its own state check fails). There's genuinely nothing to
         * switch to, so don't pay for a pointless save/restore through
         * PendSV just to land back in the exact same place. */
        return;
    }

    current_index = (uint32_t)next;
    kernel_switch_to(task_list[current_index]);
}

void kernel_start(void)
{
    /* kernel_init() (kernel/task.c) sets PendSV's priority to the
     * lowest in the system. Stage 4 needs SysTick's priority pinned down
     * too - see this file's header comment on why PendSV must only ever
     * run once SysTick (and everything else) has already finished. */
    kernel_init();

    /* SysTick counts DOWN from SYST_RVR to 0, then reloads and fires.
     * "Ticks per period" = SystemCoreClock / TICK_HZ; the register wants
     * that minus 1, because a reload of N counts N+1 cycles (N, N-1, ...,
     * 0 is N+1 values). At 84MHz/1000Hz that's 84000 cycles/ms, so
     * SYST_RVR = 83999. See docs/tick_scheduler.md for this worked out
     * in full, including why using SystemCoreClock here (rather than a
     * second hard-coded 84000000) is the entire reason
     * boot/system_stm32f4.h exists. */
    SYST_RVR = (SystemCoreClock / TICK_HZ) - 1UL;
    SYST_CVR = 0;   /* clear current value (and COUNTFLAG) before starting, for a clean first tick */
    SYST_CSR = SYST_CSR_CLKSOURCE | SYST_CSR_TICKINT | SYST_CSR_ENABLE;

    /* Bootstrap the very first switch - identical reasoning to Stage 3's
     * kernel_start_first_task(): current_task=0 means "nothing to save",
     * this bootstrap context (kernel_start() itself, and main() before
     * it) is thrown away for good once PendSV fires. */
    current_task = 0;
    current_index = 0;
    slice_remaining = TIME_SLICE_TICKS;
    kernel_switch_to(task_list[0]);

    for (;;) {
        /* Safety net only - see kernel/task.c's kernel_start_first_task()
         * (Stage 3) for why this loop exists and why it's normally
         * unreachable. */
    }
}

void SysTick_Handler(void)
{
    if (num_tasks == 0) {
        return;   /* nothing to schedule - shouldn't happen if kernel_start() was used correctly */
    }

    if (slice_remaining > 0) {
        slice_remaining--;
    }

    if (slice_remaining == 0) {
        slice_remaining = TIME_SLICE_TICKS;

        /* This is the ENTIRE forced-preemption "switch": decide who's
         * next (skipping anyone TASK_BLOCKED) and hand that decision to
         * the mechanism layer. scheduler_yield() is the exact same
         * function kernel/sem.c's sem_wait() calls when a task
         * voluntarily blocks - the timer-driven and the sync-primitive-
         * driven paths both funnel through one place that decides "who's
         * next", which is what keeps this file's mechanism/policy split
         * intact even with two different reasons to reschedule now. */
        scheduler_yield();
    }
}
