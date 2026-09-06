# AniRTOS

A real-time operating system for ARM Cortex-M4F, built from scratch —
no CMSIS device headers, no HAL, no third-party kernel. Target hardware:
**NUCLEO-F446RE** (STM32F446RET6). This is a learning project: every
stage is meant to be read and understood, not just compiled.

## Status: Stage 5b complete — task priorities and immediate preemption

Stage 5 (synchronization primitives) is big enough to be built in
sub-stages: 5a landed blocking/waking + a counting semaphore; 5b (this
one) adds task priorities and preemption that happens the instant it's
warranted, not at the next tick; 5c will add priority-inheriting mutexes
(and the priority-inversion problem they solve); 5d message queues.

Stage 5a added the ability for a task to genuinely have nothing to
do — marked `TASK_BLOCKED` and skipped by the scheduler entirely until
something wakes it back up. Stage 5b builds on that: not every task is
equally important, and a high-priority task waking up now takes the CPU
immediately, rather than waiting its turn behind whoever's currently
running.

**Stage 1 (bare-metal bring-up)** — proof the chip boots, runs your code,
and can toggle a pin:

- `boot/startup_stm32f446xx.S` — vector table (102 entries: 16 core +
  86 peripheral IRQs) + `Reset_Handler` + default exception/IRQ handlers
- `boot/stm32f446re.ld` — linker script (512K flash / 128K RAM memory map)
- `boot/system_stm32f4.c` — `SystemInit()`: HSE 8MHz → PLL → 84MHz
- `boot/stm32f446_regs.h` — hand-written register definitions (RCC, GPIO, PWR, FLASH)
- `drivers/gpio.c` — minimal GPIO driver

Verified with `objdump`/`nm`/`readelf`: vector table word 0 is
`0x20020000` (`_estack`), word 1 is `Reset_Handler` with the Thumb bit
set, and `readelf -A` confirms `ARMv7E-M` + `VFPv4-D16`. See
`docs/cortex_m_boot.md`.

**Stage 2 (MSP/PSP split)** — `main()` now moves itself off the boot-time
Main Stack and onto a dedicated Process Stack before doing anything else:

- `boot/stm32f446re.ld` — now carves a 1K Main Stack region and a 4K
  Process Stack region out of the top of RAM (`_process_stack_top`),
  with a link-time `ASSERT` guarding against overlap with `.bss`
- `kernel/context_switch.S` — `SwitchToPSP()`: sets PSP, sets
  `CONTROL.SPSEL`, `ISB`
- `app/main.c` — calls `SwitchToPSP()` as its first action

Verified statically and confirmed on real hardware via live gdb register
inspection (`CONTROL`/`PSP`/`MSP` before and after `SwitchToPSP()`
matched the predicted values bit for bit — see `docs/msp_psp_exc_return.md`).

**Stage 3 (task control blocks & the first context switch)** — two real
tasks, hand-crafted initial contexts, switched by a real handler:

- `kernel/kernel.h` — a minimal `TCB_t` (one field: a saved stack
  pointer — must be first, see the header's comment on why)
- `kernel/task.c` — `task_init()` hand-crafts a 16-word fake "interrupted"
  stack frame per task; `kernel_switch_to()`/`kernel_start_first_task()`
  pend `PendSV` via `SCB->ICSR`; `kernel_init()` sets `PendSV` to the
  lowest exception priority
- `kernel/pendsv.S` — `PendSV_Handler`: saves R4-R11 to the outgoing
  task's stack (skipped on the very first switch — nothing to save
  yet), restores R4-R11 from the incoming task's stack, updates
  `current_task`, and lets hardware's own exception-return mechanism pop
  the rest
- `app/main.c` — two tasks with deliberately different rhythms (three
  fast LED flickers vs. one long glow) specifically so a broken switch
  produces a *visibly different* failure than a working one, not just
  "nothing happens" — see `docs/context_switch.md`'s failure-mode section

Verified statically down to the instruction level (disassembly of both
`task_init()` and `PendSV_Handler` matches the intended semantics
exactly, including the literal-pool addresses of `current_task`/
`next_task`, and the reported RAM usage matches the two stacks + TCBs
exactly). Functional verification on real hardware — the flicker/glow
pattern, plus confirming `r0`/`current_task` live via
`arm-none-eabi-gdb` — confirmed this stage was solid before moving on;
see `docs/context_switch.md` for the full walkthrough and recipe.

**Stage 4 (the tick and preemptive scheduling)** — a timer now decides
who runs, not the tasks themselves:

- `boot/system_stm32f4.h` — exposes `SystemInit()` and
  `extern uint32_t SystemCoreClock` so the scheduler can compute SysTick's
  reload value from the real clock speed instead of a second hard-coded
  constant
- `kernel/scheduler.c` — `SysTick_Handler` (fires every 1ms, decrements a
  per-task time-slice countdown, and only when a slice expires picks the
  next task round-robin and *pends* `PendSV` — it never performs the
  switch itself, see the file's header comment and
  `docs/tick_scheduler.md` for exactly why that would be unsafe once
  other interrupts exist), `scheduler_add_task()`, `kernel_start()`
  (replaces Stage 3's `kernel_start_first_task()` — configures SysTick,
  then bootstraps the first task)
- `kernel/kernel.h` / `kernel/task.c` — `kernel_init()` now also pins
  SysTick's priority to the lowest in the system, tied with `PendSV`
  (deliberately equal, not just both low — see `docs/tick_scheduler.md`)
- `app/main.c` — both demo tasks are now fully preemptive: neither calls
  `kernel_switch_to()` on itself any more. Two `volatile` counters
  (`task_a_runs`, `task_b_runs`) are the real verification signal now,
  since genuine preemption makes Stage 3's clean visual rhythm
  impractical to rely on — `task_b` deliberately never touches the LED,
  so its counter's growth can't be an artifact of anything visible

Verified statically (disassembly confirms `kernel_start()` computes
SysTick's reload from `SystemCoreClock` rather than a hard-coded literal,
and that `SysTick_Handler` only ever calls `kernel_switch_to()` — never
touches PSP or R4-R11 directly). Functional verification on real
hardware: both `task_a_runs` and `task_b_runs` climbing when sampled live
via gdb (using Ctrl-C to interrupt the free-running target, since this
stage has no single deterministic breakpoint the way Stages 2-3 did) —
see `docs/tick_scheduler.md` for the full recipe.

**Stage 5a (blocking, waking, and the first semaphore)** — a task can now
genuinely have nothing to do:

- `kernel/kernel.h` — `TCB_t` gains `state` (`TASK_READY`/`TASK_BLOCKED`)
  and `next_waiter` (an intrusive linked-list pointer, threaded directly
  through TCBs — no separate waiter data structure needed)
- `kernel/scheduler.c` — `pick_next_ready()` walks the task list skipping
  anyone `TASK_BLOCKED`; `scheduler_yield()` is the one place "who runs
  next" is decided, called both by `SysTick_Handler` (forced, periodic)
  and by a task blocking on a semaphore (voluntary, immediate) — same
  mechanism/policy split as Stage 4, one layer up
- `kernel/sem.h` / `kernel/sem.c` — a counting semaphore: `sem_wait()`
  (fast path if a unit's available; otherwise blocks and yields),
  `sem_post()` (direct hand-off to a waiting task, or banks a unit if
  nobody's waiting) — both protected by `cpsid i`/`cpsie i` critical
  sections (a blunt stopgap; Stage 7 refines this)
- `app/main.c` — `task_producer` (never blocks) periodically
  `sem_post()`s; `task_consumer` spends nearly all its life blocked in
  `sem_wait()`, toggling the LED only when woken. `producer_posts` and
  `consumer_wakes` are the verification signal — they should track each
  other closely no matter how differently timed the two tasks are

Verified statically (disassembly confirms `cpsid`/`cpsie i` genuinely
bracket both semaphore functions, and `task_init()` correctly initializes
the new TCB fields at their expected offsets). Functional verification on
real hardware: `producer_posts`/`consumer_wakes` climbing together live
via gdb, and `current_task`/`tasks[1].state` confirming the consumer is
really `TASK_BLOCKED` between wakeups, not busy-waiting — see
`docs/blocking_and_semaphores.md` for the full recipe. Note: this project
has no idle task yet (Stage 10), so the demo deliberately keeps
`task_producer` always runnable — see the same doc for why.

**Stage 5b (task priorities and immediate preemption)** — not every task
is equally important, and now the scheduler knows it:

- `kernel/kernel.h` — `TCB_t` gains `priority` (`TASK_PRIORITY_LOW` /
  `NORMAL` / `HIGH` — higher number wins, the OPPOSITE convention from
  ARM's own hardware exception priorities used elsewhere in this project,
  documented explicitly to avoid mixing them up); `task_init()` takes a
  priority argument
- `kernel/scheduler.c` — `pick_next_ready()` becomes a two-pass search:
  find the highest priority among `TASK_READY` tasks, then round-robin
  only among tasks at that tier — priority breaks ties across tiers,
  round-robin still breaks ties within one
- `kernel/sem.c` — `sem_post()` now calls `scheduler_yield()` immediately
  after waking a task (only on that path, never on the "just bank a
  unit" path) — this is what makes priority-based preemption real: a
  high-priority task that was just woken runs *right now*, not whenever
  the next `SysTick` tick happens to land
- `app/main.c` — `task_low` (never blocks, doubles as the "someone must
  always be ready" task) periodically posts to a semaphore; `task_high`
  blocks almost all the time and should take over the instant
  `task_low` posts

Verified statically (disassembly confirms the two-pass priority search
in `pick_next_ready()`, and that `sem_post()` only calls
`scheduler_yield()` on the wake path). Functional verification on real
hardware used a targeted breakpoint at the exact `scheduler_yield()` call
inside `sem_post()` to prove the switch to the high-priority task happens
synchronously, inside `sem_post()` itself — not "eventually" — see
`docs/priority_scheduling.md` for the full recipe and the reasoning
behind the higher-number-wins convention.

## Building

```
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
cmake --build build
```

Produces `build/anirtos.elf`, `build/anirtos.bin`, `build/anirtos.hex`,
and prints a FLASH/RAM usage summary.

## Flashing (real hardware, onboard ST-Link over USB)

```
cmake --build build --target flash
```

This runs `openocd -f openocd.cfg -c "program ... verify reset exit"`.
The Nucleo's onboard ST-Link/V2-1 needs no separate wiring — just the USB
cable. See `docs/hardware_notes.md` for troubleshooting if the LED
doesn't blink after flashing. **Flashing needs the physical board
connected to your machine** — run this on your own Mac, not inside a
cloud sandbox that can't see your USB devices.

## Roadmap — where this project is going

Each stage below builds directly on the previous one. Nothing is skipped:
by the time the scheduler exists, you'll have personally traced through
every mechanism it depends on.

1. **Bare-metal bring-up** ✅ — boot, linker script, clocks, GPIO.
2. **Privilege levels & dual-stack model** ✅ — split Main Stack Pointer
   (MSP, used by the kernel/exceptions) from Process Stack Pointer (PSP,
   used by tasks); `CONTROL` register, `EXC_RETURN`, and why an RTOS
   needs two stack pointers before it can have more than one task. See
   `kernel/context_switch.S` and `docs/msp_psp_exc_return.md`.
3. **Task control blocks & the first context switch** ✅ — a minimal
   `TCB` (just a saved stack pointer), hand-crafted initial stack frames
   for two tasks so each looks like it was "interrupted" before it ever
   ran, and `PendSV_Handler` switching between them for real. This was
   the single most important stage conceptually — now that two tasks
   correctly switch, N tasks is just a loop. (this stage — see
   `kernel/task.c`, `kernel/pendsv.S`, `docs/context_switch.md`)
4. **The tick and cooperative→preemptive scheduling** ✅ — `SysTick_Handler`
   drives time, a simple round-robin scheduler picks the next task,
   `PendSV` is triggered from `SysTick` to actually perform the switch
   (and why the switch happens in `PendSV`, at the lowest exception
   priority, and not directly in `SysTick_Handler`). See
   `kernel/scheduler.c`, `docs/tick_scheduler.md`.
5. **Synchronization primitives** — in progress, built as sub-stages:
   - 5a ✅ blocking/waking + a counting semaphore — see `kernel/sem.c`,
     `docs/blocking_and_semaphores.md`
   - 5b ✅ task priorities + immediate priority-based preemption (this
     stage — see `kernel/scheduler.c`, `docs/priority_scheduling.md`)
   - 5c mutexes with priority inheritance (and *why* naive mutexes cause
     priority inversion)
   - 5d message queues
6. **SVC-based syscalls** — move kernel entry points behind `SVC`
   instructions instead of calling kernel functions directly from task
   code, so user tasks and kernel code have a real privilege boundary
   (relevant if/when tasks run unprivileged).
7. **Interrupt-safe kernel primitives** — critical sections via
   `BASEPRI`/`PRIMASK`, ISR-safe versions of semaphore/queue APIs
   (`...FromISR`), and NVIC priority grouping so the kernel's own
   critical sections don't accidentally block hard-real-time interrupts.
8. **Memory protection (MPU)** — per-task stack guard regions at minimum;
   optionally full task memory isolation.
9. **Drivers as first-class kernel citizens** — a UART driver built on
   the primitives from stage 5 (not busy-waiting), used for a debug
   console / shell.
10. **Power management** — idle task enters sleep (`WFI`) instead of
    spinning, tickless idle for battery-sensitive use cases.

Stages 2–4 are the conceptual core of "what is an RTOS kernel actually
doing" — everything after that is refinement and features.

## Design decisions and why

- **No CMSIS/HAL.** Every register access in this project is written by
  hand against the STM32F446 reference manual (RM0390) so nothing is
  hidden behind a macro you haven't read. This is slower to write and
  intentionally so.
- **CMake, not raw Makefiles.** Mainly for `--target flash` and clean
  toolchain-file cross-compilation; the underlying build is still just
  `arm-none-eabi-gcc`/`ld` under the hood — nothing CMake-specific is
  load-bearing to the RTOS concepts.
- **84MHz, not the chip's 180MHz max.** Reaching 180MHz requires the PWR
  peripheral's over-drive sequence on top of plain voltage scaling; 84MHz
  needs neither, and is the same standard PLLM=8/N=336/P=4 configuration
  used as the default in Zephyr and other STM32F4 bring-up examples. See
  `boot/system_stm32f4.c` for the full reasoning — pushing to 180MHz is a
  reasonable exercise once you have a way to debug it (a later stage).
- **`-mfloat-abi=hard`.** The STM32F446 has a real FPU; Stage 1 doesn't
  use floats, but any C code that does will get real FPU instructions,
  and the RTOS's context-switch code (Stage 3) has to account for that
  from the start rather than bolt it on later. See
  `docs/cortex_m_boot.md` section 6.

## Project layout

```
boot/     startup code, linker script, clock init, register definitions
drivers/  peripheral drivers (GPIO now; UART etc. later)
kernel/   MSP/PSP switch, TCB, PendSV context switch, SysTick + round-robin scheduler, sync primitives (sem.c, more to come in 5b-5d)
app/      the application/demo task(s)
cmake/    toolchain file
docs/     deep-dive explanations per stage
```
