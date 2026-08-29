/*
 * SwitchToPSP() - move execution from the Main Stack (MSP) onto the
 * Process Stack (PSP), once, from Thread mode. Called exactly once, from
 * main(), as the very first thing it does.
 *
 * This is deliberately NOT yet a real context switch (that's Stage 3,
 * and it happens inside PendSV_Handler, not by a function a task calls
 * on itself). This is the simplest possible demonstration of the
 * mechanism a real switch is built from: two stack pointer registers
 * exist in the core, and one bit picks which one is "live".
 *
 * ============================================================
 * The CONTROL register
 * ============================================================
 * CONTROL is a 2-bit-relevant special register (not memory-mapped -
 * accessed only via MSR/MRS instructions):
 *   bit 0 (nPRIV) - 0 = privileged Thread mode, 1 = unprivileged.
 *                   We leave this 0 (still privileged) - unprivileged
 *                   tasks are a later stage (memory protection), not
 *                   needed just to have separate stacks.
 *   bit 1 (SPSEL) - 0 = use MSP in Thread mode, 1 = use PSP in Thread
 *                   mode. THIS is the bit that matters here.
 *
 * Critical fact for later stages: SPSEL only has any effect in Thread
 * mode. Handler mode (any exception/interrupt handler) ALWAYS uses MSP,
 * full stop, regardless of what CONTROL says. That's not a convention -
 * it's how the hardware works. It's what makes this whole scheme safe:
 * no matter how many tasks come and go on PSP, or how badly one of them
 * mismanages its own stack, every exception handler still gets a clean,
 * known-good MSP to run on.
 *
 * ============================================================
 * Why the ISB right after MSR CONTROL
 * ============================================================
 * Cortex-M cores may fetch/decode several instructions ahead of the one
 * currently executing (pipelining). Writes to CONTROL are not guaranteed
 * to be visible to instructions already in that pipeline - the ARMv7-M
 * architecture reference manual explicitly requires an ISB (Instruction
 * Synchronization Barrier) immediately after an MSR to CONTROL if any
 * following instruction depends on the new value. Skip it, and you have
 * a bug that's a real function of pipeline state - notoriously the kind
 * of thing that "usually works" and then doesn't.
 */

    .syntax unified
    .cpu cortex-m4
    .thumb

    .section .text.SwitchToPSP
    .global SwitchToPSP
    .thumb_func
    .type SwitchToPSP, %function
SwitchToPSP:
    ldr r0, =_process_stack_top   /* top of the process stack, from the linker script */
    msr psp, r0                   /* PSP = top of process stack (nothing pushed onto it yet) */

    movs r0, #2                   /* CONTROL = 0b10: SPSEL=1 (use PSP), nPRIV=0 (stay privileged) */
    msr control, r0
    isb                           /* required: flush the pipeline before relying on the new stack */

    bx lr                         /* return to main() - but now executing ON the process stack:
                                    * this very `bx lr` is the last instruction that ran on MSP */
    .size SwitchToPSP, .-SwitchToPSP

/*
 * A deliberate detail: this function never does `push {..., lr}` /
 * `pop {..., pc}`. It doesn't need to (it calls nothing and uses no
 * callee-saved registers), and it MUST NOT: the call into this function
 * (`bl SwitchToPSP`) pushed nothing itself either (bl just sets LR to
 * the return address, it doesn't touch the stack), but if this function
 * had pushed anything onto entry, that push would land on the OLD stack
 * (MSP) - and a `pop` at the end, after the switch above, would then try
 * to pop from the NEW stack (PSP), which never received that data. You'd
 * jump to garbage. Any function that changes which stack is active mid-
 * flight has to either do it in a leaf function with no stack usage of
 * its own (this one), or be very deliberate about which stack each push
 * and pop actually targets. Stage 3's PendSV_Handler will face this
 * exact issue in a sharper form: it needs to manipulate BOTH stacks by
 * hand in a single handler.
 */
