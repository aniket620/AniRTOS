/*
 * Stage 5a: blocking and waking, via a counting semaphore.
 *
 * Stage 4's two tasks were both ALWAYS runnable - the scheduler's only
 * job was deciding whose turn it was among tasks that all had something
 * to do. This stage adds a task that, most of the time, genuinely has
 * NOTHING to do: task_consumer spends nearly all of its life BLOCKED,
 * and only actually runs for a handful of instructions each time
 * task_producer wakes it up.
 *
 * task_producer never blocks - it just loops on a delay and posts to a
 * semaphore periodically. That's not an arbitrary choice: this project
 * has no idle task yet (that's Stage 10), so the scheduler has nothing
 * to fall back to if EVERY task is blocked at once (see
 * kernel/scheduler.c's pick_next_ready()). Keeping task_producer always
 * TASK_READY is what this demo relies on to avoid that situation - a
 * real constraint worth being honest about, not hidden.
 *
 * task_consumer calls sem_wait() in a loop. Every time task_producer
 * posts, task_consumer wakes up, toggles the LED once, and goes right
 * back to waiting. So the LED's blink rate is now controlled entirely by
 * the PRODUCER's timing, even though it's the CONSUMER's code that
 * touches the GPIO - a real, visible demonstration that blocking/waking
 * is doing its job: task_consumer is provably not running (not spinning,
 * not polling, not burning CPU checking "is it my turn yet") for the
 * entire stretch between blinks.
 */
#include <stdint.h>
#include "gpio.h"
#include "kernel.h"
#include "sem.h"

extern void SwitchToPSP(void);

#define TASK_STACK_WORDS 64   /* 256 bytes - plenty for these tiny tasks */

static uint32_t producer_stack[TASK_STACK_WORDS];
static uint32_t consumer_stack[TASK_STACK_WORDS];
static TCB_t tasks[2];

static sem_t blink_sem;

/* The verification signal for this stage - see docs/blocking_and_semaphores.md.
 * producer_posts counts how many times task_producer called sem_post();
 * consumer_wakes counts how many times task_consumer's sem_wait() actually
 * returned. If blocking/waking is working, these two track each other
 * closely no matter how differently timed the two tasks are - that's the
 * whole point of a semaphore: it makes "signal" and "response" match up
 * exactly, without either task polling the other. */
static volatile uint32_t producer_posts = 0;
static volatile uint32_t consumer_wakes = 0;

static void delay(volatile uint32_t count)
{
    while (count--) {
        __asm__ volatile ("nop");
    }
}

static void task_producer(void *arg)
{
    (void)arg;
    for (;;) {
        delay(800000);          /* stands in for "some real event happened" - a sensor
                                  * reading, a button press, a byte arriving on a UART -
                                  * anything a later stage might trigger this from */
        producer_posts++;
        sem_post(&blink_sem);   /* never blocks; just updates count/waiters and returns */
    }
}

static void task_consumer(void *arg)
{
    (void)arg;
    for (;;) {
        sem_wait(&blink_sem);   /* returns immediately if a post is already banked in
                                  * count; otherwise this task is TASK_BLOCKED and off
                                  * the scheduler's rotation until task_producer posts */
        gpio_led_toggle();
        consumer_wakes++;
    }
}

int main(void)
{
    SwitchToPSP();  /* Stage 2's switch - still the required first step. See docs/context_switch.md. */
    gpio_led_init();

    sem_init(&blink_sem, 0);   /* starts at 0 - the very first sem_wait() should genuinely
                                 * block until the first sem_post(), not fall through */

    task_init(&tasks[0], producer_stack, TASK_STACK_WORDS, task_producer, 0);
    task_init(&tasks[1], consumer_stack, TASK_STACK_WORDS, task_consumer, 0);

    scheduler_add_task(&tasks[0]);
    scheduler_add_task(&tasks[1]);

    kernel_start();   /* never returns */

    return 0; /* unreachable */
}
