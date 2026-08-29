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

static TCB_t *ready_list[MAX_TASKS];
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
    ready_list[num_tasks] = tcb;
    num_tasks++;
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
    kernel_switch_to(ready_list[0]);

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

        if (num_tasks > 1) {
            current_index = (current_index + 1) % num_tasks;

            /* This is the ENTIRE "switch": decide who's next, hand that
             * decision to the mechanism layer via the exact same
             * function a task used to call on itself in Stage 3.
             * kernel_switch_to() just sets next_task and pends PendSV -
             * it does not touch PSP, R4-R11, or anything else that would
             * only be safe to touch once nothing else is mid-flight. The
             * actual switch happens later, in PendSV_Handler, once this
             * handler (and anything it might itself be nested inside of)
             * has fully returned. See this file's header comment. */
            kernel_switch_to(ready_list[current_index]);
        }
        /* num_tasks == 1: nothing to switch TO - let the one task keep
         * running. Round-robin among one task is a no-op by definition. */
    }
}
