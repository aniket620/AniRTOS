/*
 * Stage 4: two tasks, now under a real preemptive scheduler.
 *
 * The biggest change from Stage 3 isn't in this file - it's what's
 * MISSING from it. Neither task_a() nor task_b() below ever calls
 * kernel_switch_to() any more. In Stage 3 that call was how a task gave
 * up the CPU, voluntarily, on its own schedule - "cooperative"
 * multitasking, and the LED's flicker/glow rhythm was a direct, visible
 * echo of exactly when each task chose to hand off. Here, both tasks
 * just loop forever as if they owned the CPU outright. kernel/scheduler.c's
 * SysTick_Handler is the one deciding when each task gets interrupted and
 * swapped out - neither task has any say in it, or even any way to know
 * it's happening. That's what "preemptive" means.
 *
 * One consequence worth being honest about: task_a's LED rhythm is no
 * longer a clean, deliberately-designed pattern the way Stage 3's was.
 * It blinks at whatever rate its own delay loop produces, DIVIDED by
 * however evenly the scheduler happens to be sharing the CPU with
 * task_b - genuinely preemptive scheduling trades away that kind of
 * crisp visual signature for real, timer-driven fairness. So this stage
 * leans on a different, stronger verification signal instead: two
 * counters, incremented once per loop iteration by each task, read live
 * with a debugger. If both counters are climbing, both tasks are
 * genuinely getting CPU time - which is the actual property being
 * tested, independent of what either task's code happens to look like.
 * task_b deliberately never touches the LED at all, specifically so
 * nothing about its counter's growth could be an artifact of anything
 * visible - see docs/tick_scheduler.md for the full verification recipe.
 */
#include <stdint.h>
#include "gpio.h"
#include "kernel.h"

extern void SwitchToPSP(void);

#define TASK_STACK_WORDS 64   /* 256 bytes - plenty for these tiny tasks */

static uint32_t task_a_stack[TASK_STACK_WORDS];
static uint32_t task_b_stack[TASK_STACK_WORDS];
static TCB_t tasks[2];

/* The real verification signal for this stage - see the file header
 * comment. `volatile` because a debugger reads these asynchronously
 * (from `continue`'d, free-running code, via Ctrl-C) and because nothing
 * about a plain read/increment would otherwise stop the compiler from
 * reordering or eliminating it. */
static volatile uint32_t task_a_runs = 0;
static volatile uint32_t task_b_runs = 0;

static void delay(volatile uint32_t count)
{
    while (count--) {
        __asm__ volatile ("nop");
    }
}

static void task_a(void *arg)
{
    (void)arg;   /* (void*)1 here - see main(); still genuinely delivered via task_init()'s R0,
                  * same as Stage 3 - task_init() didn't change. */
    for (;;) {
        gpio_led_toggle();
        delay(400000);
        task_a_runs++;
        /* No kernel_switch_to() here any more - task_a just keeps
         * looping. Whether it actually gets to run the NEXT iteration
         * right away, or gets preempted first and resumes later, is
         * entirely SysTick_Handler's decision, not this code's. */
    }
}

static void task_b(void *arg)
{
    (void)arg;   /* (void*)2 here */
    for (;;) {
        task_b_runs++;
        /* Deliberately touches nothing visible - see file header. A
         * short delay here isn't load-bearing for correctness, just
         * keeps this loop from being a completely trivial one-instruction
         * spin. */
        delay(1000);
    }
}

int main(void)
{
    SwitchToPSP();  /* Stage 2's switch - still the required first step: PendSV's very first
                      * entry needs CONTROL.SPSEL=1 (Thread+PSP) already set. See docs/context_switch.md. */
    gpio_led_init();

    /* kernel_init() is no longer called from here directly - kernel_start()
     * (kernel/scheduler.c) now calls it internally, right alongside the
     * SysTick setup that belongs next to it. See kernel/kernel.h. */

    task_init(&tasks[0], task_a_stack, TASK_STACK_WORDS, task_a, (void *)1);
    task_init(&tasks[1], task_b_stack, TASK_STACK_WORDS, task_b, (void *)2);

    /* Register both tasks with the round-robin scheduler instead of
     * hand-picking switch targets the way Stage 3's tasks did. Order
     * here just fixes the initial turn order. */
    scheduler_add_task(&tasks[0]);
    scheduler_add_task(&tasks[1]);

    kernel_start();   /* never returns - configures SysTick, bootstraps task_a, and from
                        * here on the timer alone decides who runs */

    return 0; /* unreachable */
}
