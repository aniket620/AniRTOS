# The first context switch — Stage 3

This is the stage everything else in an RTOS sits on top of. Stage 1
proved the chip boots; Stage 2 proved a task can run on its own stack;
this stage proves you can have *two* tasks and correctly move the CPU
between them — and once that's true, N tasks is just a loop.

## The core idea in one sentence

A "task" is nothing but a stack, with a saved stack pointer telling you
where on it the task's register state currently lives — and switching
tasks means: save the outgoing task's registers onto its own stack,
remember where that ended up, then do the reverse for the incoming task.

## The Task Control Block

```c
typedef struct TCB {
    uint32_t *sp;
} TCB_t;
```

One field. `sp` must be the *first* field — `kernel/pendsv.S` treats a
`TCB_t*` as a plain pointer to a `uint32_t*`, with zero knowledge that a
struct is even involved. This is real, not a toy simplification: FreeRTOS's
own TCB has the identical constraint on its first member, for the
identical reason.

## The frame layout — what makes the trick work

Cortex-M hardware automatically pushes 8 registers onto whichever stack
is active, on every exception entry: `{R0, R1, R2, R3, R12, LR, PC,
xPSR}`, in that order, low address to high. `PendSV_Handler` additionally
saves the 8 registers hardware *doesn't* touch — `{R4-R11}` — just below
that, by hand. So a task's full saved context, sitting on its own stack,
looks like this (low address at top):

| Offset from `sp` | Register | Who wrote it | Meaning |
|---|---|---|---|
| +0  | R4  | PendSV (software) | callee-saved |
| +4  | R5  | PendSV (software) | callee-saved |
| +8  | R6  | PendSV (software) | callee-saved |
| +12 | R7  | PendSV (software) | callee-saved |
| +16 | R8  | PendSV (software) | callee-saved |
| +20 | R9  | PendSV (software) | callee-saved |
| +24 | R10 | PendSV (software) | callee-saved |
| +28 | R11 | PendSV (software) | callee-saved |
| +32 | R0  | hardware (auto) | 1st argument |
| +36 | R1  | hardware (auto) | scratch |
| +40 | R2  | hardware (auto) | scratch |
| +44 | R3  | hardware (auto) | scratch |
| +48 | R12 | hardware (auto) | scratch |
| +52 | LR  | hardware (auto) | return address if the task function returns |
| +56 | PC  | hardware (auto) | **where execution resumes** |
| +60 | xPSR | hardware (auto) | flags, including the Thumb state bit |

`task_init()` (`kernel/task.c`) builds this exact 16-word layout by hand,
in a plain array, before the task has ever run — filling in R0 (the
task's argument), LR (a trap in case the task function ever returns), PC
(the task's entry point), and xPSR (`0x01000000` — the T bit, bit 24,
set; skip this and the very first instruction the CPU tries to execute
faults, since there's no ARM mode on this core to fall back to). R4-R11
are zeroed since there's no prior state to preserve.

This is the entire trick, stated precisely: **`PendSV_Handler`'s restore
path cannot tell, and does not need to tell, the difference between a
task resuming after really running and one starting for the very first
time.** Both are just "a saved context sitting on a stack, pointed to by
a TCB". `task_init()` forges the first kind well enough to pass as the
second.

## `PendSV_Handler`, walked through

```
mrs  r0, psp              ; r0 = current PSP (base of hardware frame)
ldr  r1, =current_task
ldr  r2, [r1]              ; r2 = current_task (TCB*, or NULL)
cbz  r2, restore            ; NULL -> first-ever switch, nothing to save
stmdb r0!, {r4-r11}         ; push R4-R11 below the hardware frame
str  r0, [r2]               ; current_task->sp = r0
restore:
ldr  r1, =next_task
ldr  r2, [r1]               ; r2 = next_task
ldr  r0, [r2]               ; r0 = next_task->sp
ldmia r0!, {r4-r11}          ; pop R4-R11
msr  psp, r0                 ; PSP = base of the hardware half
ldr  r1, =current_task
str  r2, [r1]                 ; current_task = next_task
bx   lr                        ; hardware pops {R0-R3,R12,LR,PC,xPSR} from
                                ; the NEW psp - execution resumes there
```

Two details worth being deliberate about:

- **The `current_task == NULL` check** exists for exactly one moment in
  the program's life: the very first switch, triggered by
  `kernel_start_first_task()`. There is no real "previous task" to save
  at that point — `main()`'s own execution up to that call was never
  registered as a task, and never will be again. Skipping the save isn't
  a special case bolted on; it's the natural consequence of "there's
  nothing there to save".
- **`bx lr` at the end is the entire "return".** No `pop {pc}`, no
  cleanup. The real CPU register `LR` was set by *hardware* to
  `0xFFFFFFFD` (EXC_RETURN: "Thread mode, use PSP") the instant PendSV
  was entered — this handler never touches it, and must not, or the
  return goes somewhere wrong. See `docs/msp_psp_exc_return.md` for the
  full EXC_RETURN mechanism this depends on.

## Why `SwitchToPSP()` (Stage 2) is still required

`main()` still calls it, first thing, before anything else. Here's why
it's not obsolete: PendSV's very first entry needs `CONTROL.SPSEL = 1`
(Thread mode using PSP) to already be true *before* it fires — otherwise
the hardware pushes that first (discarded) frame onto MSP instead, and
loads `LR` with `0xFFFFFFF9` (return to MSP) instead of `0xFFFFFFFD`.
Every switch after that point would then be returning to the wrong
stack. Stage 2's work turns out to be exactly the precondition Stage 3
needed all along.

## What `kernel_switch_to()` actually means at the C level

```c
void kernel_switch_to(TCB_t *next)
{
    next_task = next;
    SCB_ICSR = SCB_ICSR_PENDSVSET;
    __asm__ volatile ("dsb");
    __asm__ volatile ("isb");
}
```

This function *returns*, in the ordinary C sense — but when it returns
depends on what's calling it. `task_a` calls `kernel_switch_to(&tasks[1])`
and that call returns... after Task B has run its entire body and called
`kernel_switch_to(&tasks[0])` in turn. From Task A's point of view, one
function call took an arbitrarily long time and something else ran in
the middle. That "something else ran in the middle, then we picked up
exactly where we left off" property is what a context switch *is*, at
the level a task's own code experiences it.

One more honest detail: nothing here guarantees PendSV fires on the
*exact* next instruction after `isb` — the `dsb`/`isb` pair makes it
essentially immediate on real hardware, but the architecture doesn't
promise zero extra instructions execute first. This doesn't matter for
correctness: whenever PendSV does fire, it saves the calling task's
state exactly as it is at that moment, and that's a perfectly valid
point to resume from later. Correctness here never depends on precise
timing — only that PendSV eventually fires, which it reliably does.

## What a broken switch would actually look like

`app/main.c`'s two tasks were written with a deliberately different
rhythm (Task A: three fast flickers; Task B: one long glow) specifically
so failure modes are distinguishable, not just "nothing happens":

- **Continuous fast flickering, no long glow, ever** — the switch to
  Task B never actually happens. Most likely cause: `PendSV_Handler`'s
  strong definition in `kernel/pendsv.S` didn't actually override Stage
  1's weak `bx lr` stub (wrong section/symbol name, file not added to
  `CMakeLists.txt`), so the pended interrupt does effectively nothing.
- **LED frozen solid (on or off), no blinking at all** — likely a hard
  fault. Check `task_init()`'s frame construction first (a wrong xPSR
  T-bit, or PC without the Thumb bit, both fault immediately on the
  first attempted switch into that task).
- **Everything looks right but is oddly janky/inconsistent** — worth
  checking that `kernel_init()` actually ran (PendSV priority) and that
  `SwitchToPSP()` still runs first in `main()`.

## Verifying it on real hardware

Visual: you should see the "flicker-flicker-flicker, long glow" pattern
repeat indefinitely. That alone is decent evidence, but a debugger gives
much stronger proof, the same way it did in Stage 2:

```
break task_a
continue
info registers r0
```

`r0` should read `0x1` — the exact value `main()` passed as `task_a`'s
`arg`, delivered all the way from `task_init()`'s hand-built frame,
through PendSV's hardware-triggered restore, into a live register. Then:

```
print current_task
print *current_task
print next_task
continue
```

repeated a few times, watching `current_task` alternate between the two
`&tasks[...]` addresses, confirms the switch is genuinely alternating,
not stuck. For the deepest possible look, `break PendSV_Handler` and
`stepi` through it one instruction at a time, reading `info registers`
between steps — you can watch `psp` change value, R4-R11 get saved and
restored, and `current_task` flip, exactly as this document describes.
