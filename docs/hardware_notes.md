# Hardware notes — NUCLEO-F446RE

## No wiring needed

Unlike a bare Blue Pill + external ST-Link setup, the Nucleo-64 boards
carry their own onboard debug probe (ST-Link/V2-1) already wired to the
target MCU. Just plug the board into your Mac via the USB cable on the
ST-Link side (the connector on the "top" section of the board, above the
main MCU section) and OpenOCD will find it — no SWD wires, no external
programmer, no BOOT0 jumper to worry about.

- Onboard user LED under test: **LD2, on PA5**, active-high (see
  `drivers/gpio.c`).
- Onboard user button: **B1 (blue), on PC13** — not used in Stage 1, but
  worth knowing since PC13 is a common EXTI-interrupt teaching example
  for a later stage (button-triggered task wakeup).
- HSE (8MHz) is *not* a crystal on the main MCU on this board — it comes
  from the ST-Link's own microcontroller via its MCO output, routed in
  through solder bridges SB16 (closed) / SB50 (open) in the board's
  factory-default configuration. If someone has previously reconfigured
  those solder bridges (e.g. to run HSE from an external oscillator
  instead), `SystemInit()`'s `while (!(RCC->CR & RCC_CR_HSERDY))` loop
  in `boot/system_stm32f4.c` will spin forever. This is the one part of
  Stage 1 bring-up that depends on board configuration outside the code.

## Verifying the flash actually worked

After `cmake --build build --target flash`, LD2 should blink at roughly
1-2 Hz (the exact rate depends on the uncalibrated delay loop in
`app/main.c` — Stage 1 deliberately doesn't calibrate it against the
actual 84MHz clock yet). If it doesn't blink:

1. Confirm OpenOCD actually found the ST-Link — its startup output
   should mention `stlink` and report the target halting; if it reports
   "no device found", check the USB cable (some cables are charge-only,
   no data lines) and try a different USB port.
2. Run `openocd -f openocd.cfg -c "init; halt; reset; resume; shutdown"`
   and read OpenOCD's own console output for connection/programming
   errors.
3. If flashing succeeds but nothing happens, check whether `SystemInit`
   is stuck waiting on HSE (see the solder-bridge note above) — attach a
   debugger (`openocd -f openocd.cfg` in one terminal, `gdb-multiarch
   build/anirtos.elf -ex "target extended-remote :3333"` in another,
   then `continue` then `Ctrl-C` to break in and check the program
   counter) and see whether execution is stuck inside `SystemInit`.
