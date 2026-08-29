/*
 * Linker script for STM32F446RET6 (Cortex-M4F, 512K flash / 128K RAM),
 * as fitted on the NUCLEO-F446RE.
 *
 * Same structure as the F103 version conceptually - MEMORY regions +
 * SECTIONS placement - just bigger numbers and a different chip. SRAM on
 * this part is physically SRAM1 (112K) + SRAM2 (16K), but they're
 * contiguous from 0x20000000, so a single 128K RAM region here is the
 * standard, correct simplification (the split only matters later if you
 * care which DMA controllers can reach which half).
 */

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 512K
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);

/*
 * Stage 2: two stacks instead of one.
 *
 * Up through Stage 1, everything - Reset_Handler, SystemInit, main(),
 * the blink loop - ran on a single stack (MSP, Main Stack Pointer),
 * because there was only ever one thing running. Once there's a
 * scheduler switching between tasks (Stage 3+), each task needs its own
 * stack so switching tasks doesn't mean unwinding/corrupting whatever
 * the previous task had on its stack. Cortex-M's answer to this is built
 * into the core: two stack pointer registers, MSP and PSP, and a mode bit
 * (CONTROL.SPSEL) that picks which one is "active" in Thread mode.
 *
 * Reserved here:
 *   - Main Stack (MSP): small, used by Reset_Handler/SystemInit before
 *     the switch, and by every exception/interrupt handler afterward -
 *     Handler mode ALWAYS uses MSP regardless of CONTROL.SPSEL, which is
 *     precisely why it's safe: kernel/exception code's stack is
 *     completely isolated from whatever a task's PSP is doing.
 *   - Process Stack (PSP): what main() switches onto (see
 *     kernel/context_switch.S) and runs on from that point forward.
 *     Right now there's exactly one "task" (main()'s blink loop) using
 *     the whole region; Stage 3 carves per-task stacks out of RAM the
 *     same way, one region per TCB, instead of this single fixed block.
 *
 * See docs/msp_psp_exc_return.md for the full explanation.
 */
_Main_Stack_Size    = 0x400;  /* 1 KiB - kernel/exception stack (MSP) */
_Process_Stack_Size = 0x1000; /* 4 KiB - Stage 2's one task's stack (PSP) */

_process_stack_top = _estack - _Main_Stack_Size;

ENTRY(Reset_Handler)

SECTIONS
{
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } >FLASH

  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
    _etext = .;
  } >FLASH

  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } >RAM AT> FLASH

  .bss :
  {
    . = ALIGN(4);
    _sbss = .;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
  } >RAM

  ._user_heap :
  {
    . = ALIGN(8);
    PROVIDE(end = .);
    /* Free RAM lives here, between .bss and the stacks reserved at the
     * top of RAM - unused for now, future heap/additional task stacks. */
  } >RAM

  /DISCARD/ :
  {
    *(.ARM.exidx*)
  }
}

/* _process_stack_top - _Process_Stack_Size is the LOWEST address the
 * process stack is allowed to touch; catch a config that would make the
 * stacks overlap .bss (or each other) at link time instead of finding
 * out the hard way when a task's stack silently corrupts .bss. */
ASSERT(_process_stack_top - _Process_Stack_Size >= _ebss,
       "Stack regions overlap .bss - reduce _Main_Stack_Size/_Process_Stack_Size or check the linker script")
