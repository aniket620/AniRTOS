# MSP, PSP, CONTROL, and EXC_RETURN — Stage 2's mechanics

## Why one stack isn't enough

Through Stage 1, exactly one thing ever ran: `Reset_Handler` called
`SystemInit()` then `main()`, all using the single stack pointer the CPU
loaded at boot (MSP). One thread of execution, one stack - no conflict
possible.

The instant you want a second task, that breaks. If Task A and Task B
both used the same stack, switching from A to B mid-execution would mean
B starts pushing its own local variables and return addresses right on
top of whatever A had there - and switching back to A later would find
its stack contents overwritten. Each task needs a stack that's entirely
its own, untouched by anything else.

Cortex-M's answer is built into the core, not something an RTOS bolts on:
there are **two** stack pointer registers, MSP and PSP, and a single bit
that controls which one is "active" at any given moment.

## The two stack pointers

- **MSP (Main Stack Pointer)** — what the CPU uses out of reset (loaded
  from word 0 of the vector table), and what every exception/interrupt
  handler uses, unconditionally, the whole time this project exists.
- **PSP (Process Stack Pointer)** — a second, independent stack pointer,
  usable only in Thread mode, that doesn't exist as far as the CPU's
  reset behavior or exception handling is concerned unless you opt into
  using it.

Only one of them is "SP" (i.e. what `push`/`pop`/`bl` actually operate
on) at any instant - which one is picked by the `CONTROL` register.

## The CONTROL register

`CONTROL` isn't memory-mapped; you read/write it with `MRS`/`MSR`, not a
load/store. Two bits matter to us:

| Bit | Name  | Meaning |
|-----|-------|---------|
| 0 | nPRIV | 0 = privileged Thread mode, 1 = unprivileged. We leave this 0 — unprivileged tasks are memory-protection territory (Stage 8), not needed just to separate stacks. |
| 1 | SPSEL | 0 = SP means MSP in Thread mode. 1 = SP means PSP in Thread mode. **This is the bit `SwitchToPSP()` sets.** |

`kernel/context_switch.S`'s `SwitchToPSP()` does exactly three things:
load PSP with the top of the process stack region (`_process_stack_top`,
from the linker script), set `CONTROL = 0b10`, and execute `ISB`
immediately after — required by the architecture because the core may
have already fetched/decoded instructions after the `MSR CONTROL` before
that write takes effect; `ISB` forces a pipeline flush so nothing after
it can run on stale state.

## The fact that makes this safe: Handler mode ignores SPSEL

This is the detail that makes the whole scheme trustworthy rather than
just clever: **SPSEL only has any effect in Thread mode.** The instant
any exception fires — `SysTick`, `PendSV`, a peripheral IRQ, a fault —
the CPU is in Handler mode, and Handler mode uses MSP *unconditionally*,
no matter what CONTROL says. A task can mismanage its own PSP stack
however badly it wants; the kernel's exception-handling code still gets
a clean, correctly-sized MSP to run on, every single time. This is why
Stage 1 reserved a *separate*, protected Main Stack region instead of
just letting tasks and exceptions share one pool.

## EXC_RETURN — the piece Stage 2 sets up, Stage 3 uses

`SwitchToPSP()` performs its switch by directly writing `CONTROL` from
Thread mode — no exception involved. But the mechanism Stage 3's
`PendSV_Handler` uses to switch *between tasks* is different and more
interesting: it happens through the exception return path.

When any exception is taken while the CPU was in Thread mode, hardware
automatically pushes an 8-word frame (`xPSR`, `PC`, `LR`, `R12`,
`R3`-`R0`) onto whichever stack (MSP or PSP) was active *at the moment
the exception fired*, and loads `LR` with a special sentinel value called
**EXC_RETURN** — not a real return address. Its low byte encodes what to
do on return:

| EXC_RETURN (low byte) | Return to |
|---|---|
| `0xF9` | Thread mode, use MSP |
| `0xFD` | Thread mode, use PSP |
| `0xF1` | Handler mode (nested exception), use MSP |

When the handler finishes with a plain `bx lr`, the CPU recognizes the
top byte `0xFF` as "this is EXC_RETURN, not a code address" and pops the
matching stack frame according to that encoding, restoring `PC`/`xPSR`/
etc. and switching back to the indicated mode/stack.

Here's why that matters for a scheduler: **nothing says the CPU has to
pop the frame from the same stack it pushed the frame onto.** `PendSV`'s
handler will save the outgoing task's remaining registers onto whichever
stack the pushed frame is sitting on, switch PSP to point at a
*different* task's saved stack, and then return — and because EXC_RETURN
with `0xFD` just means "pop from whatever PSP currently is," the CPU pops
the *new* task's previously-saved context instead of the one it pushed on
entry. The task that resumes execution isn't the one that was
interrupted. That single fact — that EXC_RETURN reads PSP's *current*
value, not a value saved at exception entry — is the entire trick a
context switch is built from. `SwitchToPSP()` doesn't use this path (it's
a plain function call/return, not an exception), but understanding it now
is what makes Stage 3 legible instead of feeling like magic.

## What to verify on real hardware

Unlike Stage 1, there's nothing new to *see* — the LED blinks exactly as
before if this works, and (most likely) hard-faults silently if it
doesn't. The only way to actually confirm the switch happened is to ask
the chip directly, with a debugger:

```
# terminal 1, from ~/Documents/AniRTOS:
openocd -f openocd.cfg

# terminal 2:
arm-none-eabi-gdb build/anirtos.elf -ex "target extended-remote :3333"
```

Then, in gdb, `break main`, `continue` to stop right at `main()`'s entry
(before `SwitchToPSP()` has run), and inspect state before/after:

```
(gdb) info registers control msp psp
(gdb) next          # step over SwitchToPSP()
(gdb) info registers control msp psp
```

Before: `control` reads `0x0` and `psp` reads garbage/zero (never
initialized). After: `control` reads `0x2` (SPSEL set) and `psp` reads a
value at or just below `_process_stack_top` — you can cross-check the
exact expected address with `print/x _process_stack_top` in the same gdb
session, and `msp` should be unchanged throughout, since Stage 2 never
touches it once it's set at boot.
