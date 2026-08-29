# Cortex-M boot & exception model — the mechanics Stage 1 depends on

This is the "why", written out separately from the code comments in
`boot/startup_stm32f446xx.S` so you can read it end to end once, then treat
the code comments as reference while you work.

Everything below applies equally to Cortex-M3 and Cortex-M4 (our actual
core, on the STM32F446RE) — M4 is architecturally M3 plus DSP instructions
and an optional FPU (see section 6). The boot sequence, exception model,
and Thumb-only behavior described here don't change between the two.

## 1. There is no bootloader by default

On reset, the Cortex-M core does exactly two things, in hardware, before
a single instruction of your code runs:

1. Load a 32-bit value from address `0x00000000` into the stack pointer
   (specifically MSP — Main Stack Pointer; more on the two stack pointers
   in Stage 2).
2. Load a 32-bit value from address `0x00000004` and branch to it.

On STM32, address `0x00000000` is an alias of wherever the `BOOT0`/`BOOT1`
pins say to boot from — by default, the start of main flash,
`0x08000000`. So in practice: word 0 of your flash image is the initial
SP, word 1 is your entry point. That's `g_pfnVectors[0]` and
`g_pfnVectors[1]` in the startup file, and it's why the linker script
forces `.isr_vector` to be the very first section placed in FLASH.

**Consequence:** if your linker script places anything before
`.isr_vector`, or misaligns it, the CPU boots into garbage. This is the
single most common "my board does nothing at all" bug in bare-metal
bring-up, and it's silent — no fault, no error, just a CPU executing
whatever garbage byte pattern happened to be at the wrong address.

## 2. Thumb-only, and why addresses have their low bit set

Cortex-M4 executes **only** Thumb-2 instructions — there is no ARM
(32-bit) instruction mode at all, unlike older ARM cores. The CPU still
uses bit 0 of a branch target address as a mode flag (a holdover from
cores that supported both modes): bit 0 = 1 means "Thumb mode". Since
Thumb is the *only* mode, this bit must always be 1 in any function
pointer used as a branch target on this core, including every entry in
the vector table.

You don't set this by hand — the linker sets it automatically for any
symbol used as a function address, because it knows the symbol refers to
Thumb code. Check `arm-none-eabi-nm build/anirtos.elf`: `Reset_Handler`
shows as `08000198`, but the vector table's stored word is `08000199`
(odd) — that's the linker doing this for you. If you ever hand-craft a
function pointer at the byte level (relevant again in Stage 3, building a
fake initial stack frame for a new task), you must set this bit yourself
or the CPU will fault trying to decode ARM instructions that don't exist
on this core.

## 3. `.data`/`.bss` don't initialize themselves

C assumes, by the time `main()` runs, that every global with an
initializer already holds that value, and every global without one reads
as zero. Flash is read-only at runtime and .bss (all-zero data) isn't
even stored in the binary — writing thousands of zero bytes to the image
just to memset them at boot would be wasteful. So the linker script
places `.data`'s *load address* in FLASH (right after `.text`, via
`AT>FLASH`) but its *run address* in RAM, and `Reset_Handler` copies it
by hand before calling `SystemInit()`/`main()`. `.bss` just gets a
`memset`-equivalent loop, no copy needed. Skip this step and every
initialized global reads as whatever was in flash at that RAM's
corresponding load offset — a classic "works only sometimes, depends on
what happened to be in that RAM before" bug.

## 4. The exception model (the part that matters most for an RTOS)

Vector table entries 2 onward aren't "IRQ handler addresses" in the
generic sense — Cortex-M has a unified exception model where interrupts
(from peripherals) and internal exceptions (faults, SVC, PendSV,
SysTick) go through the *same* mechanism: the NVIC (Nested Vectored
Interrupt Controller) reads the vector table, pushes an exception stack
frame (R0-R3, R12, LR, PC, xPSR — automatically, in hardware), and
branches to the handler, all without software involvement in the
save/restore. This hardware-automatic stacking is *exactly* the
mechanism Stage 3 exploits to implement context switching: `PendSV`'s
handler doesn't need to hand-save every register on entry, because the
hardware already did it — the handler only needs to save/restore what
the hardware *doesn't* touch (R4-R11), plus swap which stack pointer is
active.

Three vector table entries are pre-wired in the startup file for exactly
this future use, currently doing nothing but `bx lr` (immediate return):

- **`SVC_Handler`** (SuperVisor Call) — triggered by the `SVC` instruction,
  used from Stage 6 onward as the entry point for kernel syscalls from
  task code.
- **`PendSV_Handler`** (Pendable Service call) — a software-triggered,
  lowest-priority exception, used from Stage 3 onward to actually perform
  context switches. It's deliberately the *lowest* priority exception so
  a switch never preempts a higher-priority interrupt that's mid-flight.
- **`SysTick_Handler`** — the periodic timer interrupt used from Stage 4
  onward as the scheduler's time base.

You don't need to understand context switching yet to understand *why*
these three exist and are already wired into the vector table: the point
of Stage 1 is that when Stage 3 needs `PendSV_Handler` to do real work,
you're modifying one weak function definition, not touching boot code,
the linker script, or the vector table again.

## 5. What to verify before trusting any of this on your hardware

This session verified statically (via `objdump`/`nm`/`readelf` on the
built ELF, no real chip available):

- Vector table word 0 = `0x20020000` = `_estack` (top of the 128K RAM
  region defined in the linker script) ✓
- Vector table word 1 = `0x08000199` = `Reset_Handler`'s address
  (`0x08000198`) with the Thumb bit set ✓
- `.isr_vector` section size = `0x198` = 408 bytes = 102 words = 16 core
  exceptions + 86 peripheral IRQs, and `Reset_Handler` lands immediately
  after it in flash (`0x08000198`) — confirms every one of the 86 IRQ
  entries transcribed from the STM32F446 vector table is present and
  nothing is off-by-one ✓
- `readelf -A` confirms the build actually targeted `ARMv7E-M` with
  `VFPv4-D16` (i.e. Cortex-M4 + the FPU flags took effect, not a
  silently-ignored typo in the toolchain file) ✓
- `Reset_Handler`'s `.data` copy loop and `.bss` zero loop disassemble to
  the expected instruction sequence ✓
- Build fits comfortably in 512K flash / 128K RAM (888 B / ~1 KB used) ✓

What can only be verified on real hardware: that HSE actually starts
(`SystemInit`'s first `while` loop spins forever if it doesn't — on this
board that would mean the ST-Link isn't actually supplying its 8MHz MCO
signal, not a soldering problem like it would be on a crystal-based
board), and that PA5 is wired to LD2 the way the Nucleo's schematic says.
See `docs/hardware_notes.md` for what to check if the LED doesn't blink.

## 6. What Cortex-M4 adds over M3 (relevant later, not in Stage 1)

Two differences worth knowing about now even though Stage 1 doesn't
exercise them:

- **DSP extensions** — single-cycle MAC instructions, SIMD-ish packed
  arithmetic. Irrelevant to an RTOS kernel itself; relevant if a task
  ever does signal processing.
- **The FPU** — this is the one that matters for kernel design. When a
  task actually uses floating point, the hardware automatically stacks
  FPU registers (S0-S15, FPSCR) on exception entry *in addition to* the
  normal R0-R3/R12/LR/PC/xPSR frame, controlled by the `FPCCR` register's
  "lazy stacking" scheme (space is reserved on the stack, but the actual
  register save is deferred until/unless the handler touches the FPU -
  an optimization so tasks that never use floats pay zero extra cost).
  Stage 3's hand-crafted initial stack frames and Stage 3's `PendSV`
  context-switch code will both need to account for this: a task that
  used the FPU needs S16-S31 saved too (those aren't auto-stacked even
  with lazy stacking on), or its floating-point state silently corrupts
  across a context switch. This project's `-mfloat-abi=hard` toolchain
  flag (see `cmake/arm-none-eabi.cmake`) means the compiler *will*
  generate FPU instructions for any C code that touches a `float` or
  `double`, so this isn't a hypothetical edge case to defer indefinitely
  — it'll matter the first time any task does floating-point math.
