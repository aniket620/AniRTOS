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
 * Task Control Block. `sp` MUST be the first member. kernel/pendsv.S
 * dereferences a TCB_t* directly as a plain address holding a uint32_t*
 * at offset 0 - it has no idea this struct exists, or what else might be
 * in it. Add a field before `sp` and PendSV_Handler silently reads/writes
 * the wrong memory, with no compiler warning anywhere. This is not a
 * hypothetical concern - FreeRTOS's own TCB has this exact same
 * constraint on its first member (`pxTopOfStack`), for the exact same
 * reason. Everything AFTER `sp`, by contrast, is free real estate -
 * nothing in the assembly ever looks at it.
 *
 * Stage 5 adds two fields, both needed the moment a task can BLOCK
 * (wait on something not yet available) instead of always being
 * immediately runnable:
 *
 *   - `state`: is this task actually eligible to run right now? Through
 *     Stage 4, every created task was always eligible - the scheduler's
 *     round-robin simply cycled through all of them. That stops being
 *     true the moment a task can call sem_wait() on an empty semaphore -
 *     it has to leave the rotation until something wakes it back up.
 *   - `next_waiter`: when a task IS blocked, something needs to remember
 *     it, so whoever eventually satisfies the wait (sem_post(), etc.)
 *     knows who to wake. Rather than a separate array-based "waiting
 *     list" data structure, this is an INTRUSIVE linked list: the "next"
 *     pointer for the list lives directly inside the TCB being linked,
 *     the same way many real kernels (including FreeRTOS) do it. A
 *     semaphore, in turn, only needs to store one thing - a pointer to
 *     the head of whichever chain of waiting TCBs is currently queued on
 *     it (see kernel/sem.h). No fixed-size array, no separate allocator -
 *     just pointers threaded through TCBs that already exist.
 */
typedef enum {
    TASK_READY   = 0,   /* eligible to be picked by the scheduler */
    TASK_BLOCKED = 1,   /* waiting on something (a semaphore, for now) - skipped by the scheduler until woken */
} task_state_t;

/*
 * Stage 5b: not every task is equally important. `priority` says how
 * much: HIGHER NUMBER = HIGHER PRIORITY (a priority-2 task always beats
 * a priority-0 task for the CPU whenever both are READY).
 *
 * Deliberately flagging this because it's the OPPOSITE convention from
 * the ARM exception priorities this project already uses everywhere
 * else (SCB_SHPR3, kernel_init() - where 0 is the HIGHEST hardware
 * priority and 0xFF is the lowest). Two genuinely different concepts
 * share the word "priority" in this codebase: hardware exception
 * priority (lower number wins - an ARM architecture convention this
 * project didn't choose) and task scheduling priority (higher number
 * wins - a convention THIS project chose, matching the more common
 * RTOS-textbook/FreeRTOS style, because "bigger number, bigger deal" is
 * the more intuitive default for application code). They never mix in
 * the same comparison, but keep the direction straight when reading
 * task.c/scheduler.c versus sem.c's critical sections.
 */
#define TASK_PRIORITY_LOW    0
#define TASK_PRIORITY_NORMAL 1
#define TASK_PRIORITY_HIGH   2

typedef struct TCB {
    uint32_t *sp;              /* MUST stay first - see above */
    task_state_t state;
    uint8_t priority;          /* higher = more important - see above */
    struct TCB *next_waiter;   /* NULL when not on any wait list; otherwise the next TCB in whichever
                                 * intrusive chain this task is currently queued on */
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
 * mid-flight - even though it never actually ran before. `priority` is
 * one of TASK_PRIORITY_LOW/NORMAL/HIGH (or any uint8_t - those are just
 * convenient names, not a hard limit) - see the note above TCB_t for the
 * higher-number-wins convention. */
void task_init(TCB_t *tcb, uint32_t *stack, uint32_t stack_words,
               task_entry_t entry, void *arg, uint8_t priority);

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

/* Stage 5: give up the CPU RIGHT NOW and let the scheduler pick whoever
 * should run next, skipping any TASK_BLOCKED task. This is the one place
 * "who runs next" is actually decided (see kernel/scheduler.c) - both
 * SysTick_Handler (forced, periodic: your time slice ran out) and
 * kernel/sem.c's sem_wait() (voluntary, immediate: you have nothing to
 * do until something wakes you) call this same function. If the caller
 * is itself still the only READY task, this is a safe no-op - see
 * kernel/scheduler.c. */
void scheduler_yield(void);

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
