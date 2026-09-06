/*
 * Stage 5b: task priorities, and preemption that happens IMMEDIATELY,
 * not just at the next tick.
 *
 * Two tasks, deliberately unequal:
 *
 *   task_low  (TASK_PRIORITY_LOW)  - the "background work" task. Never
 *     blocks - always has something to do, so it's also what keeps this
 *     project's no-idle-task constraint satisfied (see
 *     kernel/scheduler.c's pick_next_ready() comment). Periodically
 *     posts to event_sem, the way a background loop might notice
 *     something worth escalating.
 *
 *   task_high (TASK_PRIORITY_HIGH) - the "must respond right away" task.
 *     Spends nearly all its life blocked in sem_wait(), and the instant
 *     task_low posts, task_high should take the CPU IMMEDIATELY - not
 *     whenever task_low's current time slice happens to run out. That
 *     immediacy is the entire point of Stage 5b: kernel/sem.c's
 *     sem_post() now calls scheduler_yield() right after waking a task,
 *     specifically so a higher-priority task doesn't have to wait its
 *     turn the way Stage 5a's tasks did.
 *
 * Contrast with Stage 5a: there, producer and consumer had no priority
 * distinction (both defaulted to whatever "equal footing" meant before
 * priorities existed), and a wake only changed eligibility - the
 * scheduler decided when to actually act on it, sometimes tens of
 * milliseconds later. Here, task_high's priority means it's not merely
 * "eventually going to run" once woken - it wins immediately, every
 * time, over task_low.
 */
#include <stdint.h>
#include "gpio.h"
#include "kernel.h"
#include "sem.h"

extern void SwitchToPSP(void);

#define TASK_STACK_WORDS 64   /* 256 bytes - plenty for these tiny tasks */

static uint32_t low_stack[TASK_STACK_WORDS];
static uint32_t high_stack[TASK_STACK_WORDS];
static TCB_t tasks[2];

static sem_t event_sem;

/* Verification signal - see docs/priority_scheduling.md. low_runs counts
 * task_low's loop iterations (and therefore how many times it posted);
 * high_runs counts how many times task_high was actually woken and ran.
 * As in Stage 5a, these should track closely - but this stage's REAL
 * point isn't the counters, it's WHEN the switch happens (immediately,
 * provable with a targeted breakpoint - see the docs). */
static volatile uint32_t low_runs = 0;
static volatile uint32_t high_runs = 0;

static void delay(volatile uint32_t count)
{
    while (count--) {
        __asm__ volatile ("nop");
    }
}

static void task_low(void *arg)
{
    (void)arg;
    for (;;) {
        delay(800000);         /* stands in for "doing some background work" */
        sem_post(&event_sem);  /* "...and noticed something worth escalating" -
                                 * this call itself is where task_high, being
                                 * higher priority, immediately takes over */
        low_runs++;
    }
}

static void task_high(void *arg)
{
    (void)arg;
    for (;;) {
        sem_wait(&event_sem);   /* blocked almost all the time; the moment this
                                  * returns, it's because task_low just posted
                                  * and handed the CPU here right away */
        gpio_led_toggle();
        high_runs++;
    }
}

int main(void)
{
    SwitchToPSP();  /* Stage 2's switch - still the required first step. See docs/context_switch.md. */
    gpio_led_init();

    sem_init(&event_sem, 0);   /* starts empty - task_high should genuinely block
                                 * until the first post */

    task_init(&tasks[0], low_stack,  TASK_STACK_WORDS, task_low,  0, TASK_PRIORITY_LOW);
    task_init(&tasks[1], high_stack, TASK_STACK_WORDS, task_high, 0, TASK_PRIORITY_HIGH);

    scheduler_add_task(&tasks[0]);
    scheduler_add_task(&tasks[1]);

    kernel_start();   /* never returns */

    return 0; /* unreachable */
}
