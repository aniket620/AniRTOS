/*
 * Task creation and the two kernel entry points that trigger a switch
 * (kernel_switch_to, kernel_start_first_task). The actual register
 * save/restore is kernel/pendsv.S - this file only ever touches PSP
 * indirectly, by building stack CONTENTS in memory and by pending the
 * PendSV exception; it never sets PSP or CONTROL itself.
 */
#include "kernel.h"

TCB_t *current_task = 0;
TCB_t *next_task = 0;

/* SCB (System Control Block) registers - part of the Cortex-M core
 * itself, not the STM32 chip, which is why these aren't in
 * stm32f446_regs.h. */
#define SCB_ICSR            (*(volatile uint32_t *)0xE000ED04UL)
#define SCB_ICSR_PENDSVSET  (1UL << 28)   /* write 1: set PendSV pending */
#define SCB_SHPR3           (*(volatile uint32_t *)0xE000ED20UL)

static void task_exit_trap(void)
{
    /* A task's entry function returning is a bug - every task_entry_t
     * is supposed to loop forever. Trap here instead of "returning" into
     * whatever garbage address happens to sit next in memory - same
     * defensive spirit as Reset_Handler's `b .` after a returning
     * main(), or the fault handlers' `b .` stubs. */
    for (;;) {
    }
}

void kernel_init(void)
{
    /* PendSV must run at the lowest exception priority in the system: a
     * context switch must never preempt a higher-priority interrupt
     * that's mid-flight (imagine a UART IRQ getting suspended mid-byte
     * because a task happened to yield at the wrong moment). SHPR3 bits
     * [23:16] are PendSV's priority field; write the max value (0xFF -
     * the chip only implements the top few bits, hardware ignores the
     * rest, but 0xFF works regardless of exactly how many are
     * implemented). This was already true in Stage 3, before any other
     * interrupt existed to actually preempt anything - it's a
     * correctness property of PendSV itself, not something bolted on
     * once it started mattering.
     *
     * Stage 4 adds SysTick to this same rule. SHPR3 bits [31:24] are
     * SysTick's priority field - also set to 0xFF, the lowest in the
     * system, tied with PendSV. Giving SysTick and PendSV EQUAL lowest
     * priority (rather than, say, SysTick slightly higher) is
     * deliberate: equal-priority exceptions never preempt each other, so
     * if SysTick_Handler pends PendSV while it's running, PendSV simply
     * waits until SysTick_Handler fully returns before it gets to run -
     * exactly the ordering kernel/scheduler.c's header comment depends
     * on ("SysTick decides, PendSV acts", never interleaved). */
    SCB_SHPR3 |= (0xFFUL << 16) | (0xFFUL << 24);
}

void task_init(TCB_t *tcb, uint32_t *stack, uint32_t stack_words,
               task_entry_t entry, void *arg, uint8_t priority)
{
    /*
     * Build a 16-word frame at the TOP of `stack` (stacks grow down, so
     * we start from the high end), laid out EXACTLY the way
     * PendSV_Handler leaves a real saved context: R4-R11 (software-
     * saved by PendSV) at the bottom, then the 8 words the CPU hardware
     * pushes on every exception entry (R0-R3, R12, LR, PC, xPSR) above
     * them. This is the entire trick: PendSV_Handler's restore path
     * cannot tell the difference between a task resuming after really
     * running, and one starting for the very first time - both just
     * look like "a saved context sitting on this stack". See
     * docs/context_switch.md for the full frame diagram.
     */
    uint32_t *sp = &stack[stack_words];   /* one past the top - nothing lives here yet */

    sp -= 8;                             /* room for the hardware-stacked half */
    sp[0] = (uint32_t)arg;               /* R0   - AAPCS: entry()'s first argument */
    sp[1] = 0;                           /* R1   */
    sp[2] = 0;                           /* R2   */
    sp[3] = 0;                           /* R3   */
    sp[4] = 0;                           /* R12  */
    sp[5] = (uint32_t)task_exit_trap;    /* LR   - where to "return" if entry() ever returns */
    sp[6] = (uint32_t)entry;             /* PC   - execution starts here (Thumb bit set automatically,
                                           *        same linker behavior as Reset_Handler in Stage 1) */
    sp[7] = 0x01000000UL;                /* xPSR - T bit (bit 24) set: Thumb state. Skip this and the
                                           * first instruction the CPU tries to execute faults - there
                                           * is no ARM mode on this core to fall back to. */

    sp -= 8;                             /* room for the software-saved half */
    for (int i = 0; i < 8; i++) {
        sp[i] = 0;                       /* R4-R11 - don't matter; nothing ran yet to have state */
    }

    tcb->sp = sp;   /* PendSV_Handler's restore path expects this to point at the R4 slot */

    /* Stage 5a: every freshly-created task starts out eligible to run,
     * and isn't queued on anything's wait list yet. Stage 5b: and now
     * has a priority, set once here and never changed elsewhere in this
     * project (no priority-change API yet - not needed until something
     * like priority inheritance, Stage 5c, requires temporarily raising
     * one). */
    tcb->state = TASK_READY;
    tcb->next_waiter = 0;
    tcb->priority = priority;
}

void kernel_switch_to(TCB_t *next)
{
    next_task = next;
    SCB_ICSR = SCB_ICSR_PENDSVSET;
    /* DSB: make sure the write above has actually taken effect (memory-
     * mapped register writes can be posted/buffered) before continuing.
     * ISB: flush the pipeline so the now-pending exception is recognized
     * as soon as architecturally possible, instead of possibly letting a
     * few more already-fetched instructions run first. Neither is
     * strictly what makes this correct (see docs/context_switch.md - the
     * switch is correct no matter EXACTLY which instruction it takes
     * effect on), but both are standard practice and keep the switch
     * prompt. */
    __asm__ volatile ("dsb");
    __asm__ volatile ("isb");
}

/* Stage 3 had kernel_start_first_task() here: set current_task=0, call
 * kernel_switch_to(first), spin forever as a safety net. Stage 4 moves
 * that bootstrap into kernel/scheduler.c's kernel_start() instead, right
 * next to the SysTick setup it now needs to happen alongside - see that
 * file. The logic is identical; only its home (and its name) changed. */
