/*
 * PendSV_Handler - the actual context switch. Everything else in Stage 3
 * (task_init's hand-crafted frames, kernel_switch_to's ICSR write) exists
 * to set this handler up to do its job correctly. See
 * docs/context_switch.md for the full picture; this file's comments
 * cover the mechanics of these specific instructions.
 *
 * ============================================================
 * What's already true when this handler starts
 * ============================================================
 * PendSV fired while some task was executing in Thread mode on PSP
 * (guaranteed - Stage 2's SwitchToPSP() put us in Thread+PSP before the
 * very first switch, and every task since has stayed there; nothing in
 * this project ever flips CONTROL.SPSEL back). Cortex-M hardware has
 * therefore ALREADY, automatically, before this handler's first
 * instruction runs:
 *   - pushed {R0-R3, R12, LR, PC, xPSR} onto that task's PSP stack
 *   - switched to Handler mode (now using MSP - irrelevant here, this
 *     handler never touches its own stack with push/pop/bl)
 *   - loaded the real CPU register LR with EXC_RETURN = 0xFFFFFFFD
 *     (means: "on return, go to Thread mode, use PSP") - we never set
 *     this ourselves; it's already correct and we must not clobber it
 *
 * So PSP right now points at the BASE of that 8-word hardware frame
 * (i.e. at R0). This handler's job: save the other 8 registers hardware
 * doesn't touch (R4-R11) below that frame, remember where the result
 * ends up (that's "the task's saved context" from now on), then do the
 * mirror image for whichever task we're switching TO, and let `bx lr`
 * hand control back to hardware to pop that new frame.
 */

    .syntax unified
    .cpu cortex-m4
    .thumb

    .section .text.PendSV_Handler
    .global PendSV_Handler
    .thumb_func
    .type PendSV_Handler, %function
PendSV_Handler:
    mrs r0, psp                 /* r0 = current PSP = base of the hardware-stacked frame */

    ldr r1, =current_task
    ldr r2, [r1]                /* r2 = current_task (a TCB_t*, or NULL on the very first switch) */
    cbz r2, PendSV_restore      /* NULL -> nothing to save, skip straight to restoring */

    stmdb r0!, {r4-r11}         /* push R4-R11 below the hardware frame; r0 now = new lowest address */
    str r0, [r2]                /* current_task->sp = r0 (recall: sp is TCB_t's first/only member) */

PendSV_restore:
    ldr r1, =next_task
    ldr r2, [r1]                /* r2 = next_task - the task we're switching TO */
    ldr r0, [r2]                /* r0 = next_task->sp (a previously-saved SP, or task_init's fake one -
                                  * PendSV_Handler cannot tell, and does not need to, which kind it is) */
    ldmia r0!, {r4-r11}         /* restore R4-R11; r0 now points at the hardware-frame half (R0) */
    msr psp, r0                 /* PSP = that address - this is what hardware will pop from on return */

    ldr r1, =current_task
    str r2, [r1]                /* current_task = next_task: the switch is now complete */

    bx lr                       /* exception return: hardware pops {R0-R3,R12,LR,PC,xPSR} from the
                                  * NEW psp we just set, and execution resumes wherever that frame's
                                  * PC says - a real task resuming mid-loop, or task_init's fake PC
                                  * landing on a task's entry function for the very first time. Either
                                  * way, from here, hardware can't tell which one it is - and neither
                                  * did we need to. */
    .size PendSV_Handler, .-PendSV_Handler

/*
 * Notice what ISN'T here: no `push {lr}` / `pop {pc}`, no `bl` to
 * anything. Same reasoning as kernel/context_switch.S's SwitchToPSP() -
 * this handler runs on MSP (Handler mode always does) but manipulates
 * PSP by hand; mixing that with ordinary stack-based call/return on ITS
 * OWN stack would be fine in principle (MSP is untouched by any of this),
 * but every register touched here is deliberately either a scratch
 * register (r0-r2) or part of the explicit save/restore (r4-r11) - there
 * is nothing this handler needs MSP's stack for at all.
 */
