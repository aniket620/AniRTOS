/*
 * SystemInit()'s public face. Stage 1-3 never needed this - nothing
 * outside boot/system_stm32f4.c cared about the clock speed as a number.
 * Stage 4 changes that: kernel/scheduler.c has to program SysTick's
 * reload register with "however many core clock ticks make up 1ms", and
 * it can only do that correctly if it knows the real SYSCLK frequency
 * rather than hard-coding 84000000 a second time somewhere else (a
 * classic way for two numbers to silently drift apart after someone
 * changes the PLL config and forgets the copy).
 */
#ifndef SYSTEM_STM32F4_H
#define SYSTEM_STM32F4_H

#include <stdint.h>

/* HSE 8MHz -> PLL -> 84MHz SYSCLK. See boot/system_stm32f4.c for the
 * full register-by-register walkthrough. Called once, from Reset_Handler,
 * before main(). */
void SystemInit(void);

/* The real, current core clock in Hz - 84000000 after SystemInit() has
 * run. A variable, not a #define, because that's the honest CMSIS
 * convention: on a chip where the clock can change at runtime (it can't,
 * here, but the convention exists for chips where it does), code that
 * needs the clock speed reads this instead of assuming a constant. */
extern uint32_t SystemCoreClock;

#endif /* SYSTEM_STM32F4_H */
