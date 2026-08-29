/*
 * Stage 3 gave this header the mechanism (TCB_t, task_init,
 * kernel_switch_to, PendSV_Handler) - see docs/context_switch.md. Stage 4
 * adds the policy layer's public API on top: scheduler_add_task() and
 * kernel_start() replace the old "you name your own switch target"
 * model with a real timer-driven round-robin scheduler. See
 * kernel/scheduler.c and docs/tick_scheduler.md.
 */
#ifndef KERNEL_H
#define KERNEL_H

#include <stdint.h>

/*
 * Task Control Block - Stage 3's minimal version. Just enough to context-
 * switch: a saved stack pointer, and nothing else yet (no priority, no
 * ready/blocked state, no ready list - that's Stage 4's scheduler).
 *
 * `sp` MUST be the first member. kernel/pendsv.S dereferences a TCB_t*
 * directly as a plain address holding a uint32_t* at offset 0 - it has
 * no idea this struct exists, or what else might be in it. Add a field
 * before `sp` and PendSV_Handler silently reads/writes the wrong memory,
 * with no compiler warning anywhere. This is not a hypothetical
 * concern - FreeRTOS's own TCB has this exact same constraint on its
 * first member (`pxTopOfStack`), for the exact same reason.
 */
typedef struct TCB {
    uint32_t *sp;
} TCB_t;

/* current_task: whichever task PendSV_Handler is currently running "as".
 * NULL means "nothing to save" - only true for the very first switch,
 * before any real task has ever run.
 * next_task: which task the next PendSV should switch TO. Set by
 * whoever requests a switch, read by PendSV_Handler. */
extern TCB_t *current_task;
extern TCB_t *next_task;

typedef void (*task_entry_t)(void *arg);

/* One-time setup: PendSV's and SysTick's exception priorities (both must
 * be the lowest in the system - see docs/context_switch.md and
 * kernel/scheduler.c's header comment on why). Stage 4: called
 * internally by kernel_start(), not directly from main() any more. */
void kernel_init(void);

/* Hand-craft an initial stack frame for `tcb`, inside `stack` (an array
 * of `stack_words` uint32_t's), so the first time PendSV "restores" this
 * task, execution starts at `entry(arg)` as if it had been interrupted
 * mid-flight - even though it never actually ran before. */
void task_init(TCB_t *tcb, uint32_t *stack, uint32_t stack_words,
               task_entry_t entry, void *arg);

/* Request an immediate switch to `next`. Pends PendSV and returns
 * normally to the caller - but "normally" here can mean "a long time
 * later, after `next` and possibly others have run in between": that's
 * what a context switch IS at the C level. See docs/context_switch.md.
 *
 * Stage 3's demo tasks called this directly, naming their own switch
 * target - that voluntary, cooperative style still works and this
 * primitive is still exactly what it was. Stage 4's demo tasks no
 * longer call it themselves at all: kernel/scheduler.c's SysTick_Handler
 * calls it on their behalf, on a timer, whether a task "wants" to give
 * up the CPU or not. Same mechanism, different (and no longer optional)
 * policy sitting on top of it. */
void kernel_switch_to(TCB_t *next);

/* Register a task with the round-robin scheduler (kernel/scheduler.c).
 * Call once per task, after task_init(), before kernel_start(). Order
 * matters only in that it fixes the round-robin turn order. */
void scheduler_add_task(TCB_t *tcb);

/* Stage 4's replacement for Stage 3's kernel_start_first_task(): calls
 * kernel_init() internally, configures and starts SysTick, then
 * bootstraps the first task exactly as kernel_start_first_task() used
 * to. Never returns. Call once, after every task has been created with
 * task_init() and registered with scheduler_add_task(). */
void kernel_start(void);

/* Defined in kernel/pendsv.S. Never called directly from C - installed
 * into the vector table by name; its strong definition here overrides
 * Stage 1's weak default. Declared here only so its prototype exists
 * somewhere and nothing warns about an undeclared symbol. */
void PendSV_Handler(void);

/* Defined in kernel/scheduler.c. Same story as PendSV_Handler above -
 * installed into the vector table by name, overriding Stage 1's weak
 * default stub. */
void SysTick_Handler(void);

#endif /* KERNEL_H */
