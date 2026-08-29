# The tick and preemptive scheduling — Stage 4

Stage 3 proved the switch mechanism works: given two tasks and a request
to switch, `PendSV_Handler` correctly moves the CPU between them. But
every switch in Stage 3 was *requested* — a task called
`kernel_switch_to()` on itself, voluntarily, whenever it felt like it.
Stage 4 takes that choice away from the tasks entirely. A hardware timer
now interrupts the CPU on a fixed schedule, and a small piece of policy
code decides who runs next — the tasks themselves never know it's
happening. That's the actual, precise difference between *cooperative*
and *preemptive* multitasking, and it's the last conceptual piece an RTOS
needs before "scheduler" is a meaningful word to use about this project.

## SysTick, and the reload-value arithmetic

`SysTick` is a 24-bit down-counter built into every Cortex-M core (not an
STM32-specific peripheral — its registers live at a fixed address range,
`0xE000E010`–`0xE000E01C`, defined by the ARMv7-M architecture itself,
the same reasoning as `kernel/task.c`'s SCB registers). Three registers
matter here:

- `SYST_RVR` (reload value) — what the counter resets to.
- `SYST_CVR` (current value) — counts down every core clock cycle.
- `SYST_CSR` (control/status) — enable, whether to fire an interrupt on
  reaching 0, and clock source.

When `SYST_CVR` reaches 0, it reloads from `SYST_RVR` and, if `TICKINT`
is set, `SysTick_Handler` fires — automatically, forever, with zero
software intervention needed to keep it going.

The arithmetic: this project runs `SysTick` off the processor clock
(`SYST_CSR_CLKSOURCE`), which after Stage 1's `SystemInit()` is 84MHz.
`TICK_HZ` is defined as 1000 — a 1ms tick, a normal choice, fine enough
for a 20ms time slice to still mean something, coarse enough not to spend
all your CPU time servicing the timer. Ticks per period is
`SystemCoreClock / TICK_HZ` = `84000000 / 1000` = `84000` core clock
cycles per tick. `SYST_RVR` wants that number *minus one*: a reload of
`N` produces a count sequence of `N, N-1, ..., 1, 0` — that's `N + 1`
values, so a period of exactly 84000 cycles needs `SYST_RVR = 83999`.
`kernel/scheduler.c` computes this as `(SystemCoreClock / TICK_HZ) - 1`
rather than writing `83999` directly — the whole reason
`boot/system_stm32f4.h` exists is so this computation reads the *real*
clock speed from `SystemCoreClock` instead of silently duplicating the
`84000000` constant a second time somewhere a clock-speed change could
forget to update it.

`TIME_SLICE_TICKS` is a second, independent number: 20. Each task gets 20
ticks (20ms, at `TICK_HZ`=1000) before `SysTick_Handler` decides its turn
is over and picks the next one. Neither of these numbers is load-bearing
to correctness — turn `TICK_HZ` up to 10000 or `TIME_SLICE_TICKS` down to
5 and the scheduler is still exactly as correct, just switching more
often. They're policy knobs, tuned for "fast enough to look responsive,
slow enough that switching overhead is noise by comparison" — not
constants with any deeper significance.

## Why the switch still has to happen in PendSV, never in SysTick itself

This is the single point the project roadmap calls out by name, and it's
worth stating precisely rather than just asserting it.

It would compile and often even *seem* to work if `SysTick_Handler`
directly did what `PendSV_Handler` does — saved R4-R11, swapped PSP,
restored R4-R11. The bug only shows up once other interrupts exist
alongside SysTick, which is exactly the situation any real embedded
system is in (a UART RX interrupt, a button press on an EXTI line, a
timer capture — this project doesn't have one yet, but a real one always
does). Suppose one of those fires, and SysTick fires too while it's still
executing, preempting it (which SysTick, given a high enough priority,
legitimately can). If `SysTick_Handler` swapped PSP right there, the
nested interrupt's own in-flight state — its return address and
registers, sitting on the outgoing task's stack, mid-unwind — would be
corrupted or simply gone by the time hardware tries to pop it back off.
Nested exception return only works correctly when nesting is respected:
whichever handler is innermost has to be the one that finishes and
returns first, onto the exact stack it interrupted, before anything
outside it can safely resume.

`PendSV` exists specifically to be the one and only place PSP ever gets
touched, and it's deliberately configured (`kernel_init()`,
`kernel/task.c`) to run at the *lowest* priority in the entire system —
tied with SysTick itself, both at `0xFF`. Giving them equal priority
(rather than, say, SysTick strictly higher) is the deliberate detail that
makes the ordering work: exceptions at equal priority never preempt each
other. So when `SysTick_Handler` calls `kernel_switch_to()` mid-flight —
which just sets `next_task` and writes `SCB_ICSR` to *pend* PendSV, not
to run it — that pended PendSV has to wait until `SysTick_Handler` fully
returns before it becomes eligible to run at all. And by the time it
does run, by construction, nothing else in the system can still be
mid-flight underneath it: everything else already either finished or, if
it hasn't, is sitting at a priority PendSV can't preempt anyway (moot,
since PendSV is the lowest priority that exists). The switch is therefore
always safe by the time PendSV actually performs it.

Stated as a slogan: **SysTick decides, PendSV acts.** Deciding who runs
next is just picking an index into an array — cheap, and safe to do from
anywhere. Actually moving the CPU onto that choice is the part that's
only ever safe once nothing else is mid-flight, so that part is the part
deferred.

## Mechanism vs policy, concretely

- `kernel/task.c` + `kernel/pendsv.S` — **mechanism**: "given a
  `next_task` pointer, correctly move the CPU onto it." Doesn't know or
  care how `next_task` got decided.
- `kernel/scheduler.c` — **policy**: "given several tasks that all want
  to run, decide who runs next, and when." A fixed-size `ready_list`
  array, a `current_index` that walks it round-robin, and a
  `slice_remaining` tick countdown are the entirety of the policy. This
  file could be deleted and replaced with a completely different
  scheduling algorithm (priority-based, earliest-deadline-first,
  whatever a later project wants) without changing one line of
  `task.c` or `pendsv.S`.

`kernel_start()` (replacing Stage 3's `kernel_start_first_task()`) is the
seam between the two: it calls `kernel_init()` (mechanism setup — PendSV
and SysTick priorities), configures and starts the SysTick timer (policy
setup), then bootstraps the very first task exactly the way Stage 3's
version did.

## Verifying it — counters, not a clean visual pattern

Stage 3's two tasks were deliberately written with different visual
rhythms so a working switch and a broken one produced visibly different
LED behavior. That trick doesn't carry over cleanly to Stage 4: once a
task no longer controls when it gets interrupted, its own code can't
guarantee any particular visual rhythm survives contact with the
scheduler. So this stage's real verification signal is two plain
counters instead:

```c
static volatile uint32_t task_a_runs = 0;
static volatile uint32_t task_b_runs = 0;
```

`task_a` increments `task_a_runs` once per loop iteration (right after
toggling the LED); `task_b` increments `task_b_runs` once per iteration
and deliberately never touches the LED at all — specifically so nothing
about its counter's growth could be an artifact of anything visible.
If both are climbing, both tasks are genuinely getting CPU time on a
timer-driven schedule, independent of whatever either task's code
happens to look like — which is the actual property being tested.

### The gdb recipe

Unlike Stages 2-3, there's no single deterministic breakpoint to land on
and inspect — both tasks run forever, preempted on a timer, so the normal
move is to let the target run freely and interrupt it by hand:

```
target extended-remote :3333
monitor reset halt
continue
```

Let it run for a second or two, then press **Ctrl-C** in gdb — this
halts the live target wherever it happened to be, the same as hitting a
breakpoint, just at a moment you didn't pick in advance. Then:

```
print task_a_runs
print task_b_runs
continue
```

Ctrl-C again after another second or two, and repeat the two `print`s.
Both numbers should be visibly larger than last time, and neither should
ever go backwards or stay frozen while the other climbs — that's the
signature of the scheduler genuinely time-slicing between both tasks
rather than one starving the other. You can also `print current_task`
each time you stop, and watch it alternate between `&tasks[0]` and
`&tasks[1]` addresses across repeated Ctrl-C/continue cycles — direct
confirmation that the *scheduler*, not either task, is the one choosing
when that value changes.

Visually: the LED should still blink, just without Stage 3's crisp
"flicker-flicker-flicker, glow" signature — expect a less regular rhythm,
which is itself a sign this stage is doing what it's supposed to, not a
sign something's wrong.
