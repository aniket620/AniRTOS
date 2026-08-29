/*
 * SystemInit(): HSE 8MHz -> PLL -> 84MHz SYSCLK, for STM32F446RE.
 *
 * ============================================================
 * Why 84MHz and not the chip's rated 180MHz max
 * ============================================================
 * This part CAN run at 180MHz, but only by additionally enabling
 * "Over-drive mode" in the PWR peripheral (a specific enable/wait/switch
 * sequence beyond plain voltage scaling) for anything above 168MHz.
 * That's a real feature worth learning, but it adds failure modes to a
 * bring-up stage whose only job is "prove the chip boots and runs your
 * code" - and 84MHz is not a compromise pulled from nowhere: it's the
 * same PLLM=8/PLLN=336/PLLP=4 configuration Zephyr and countless other
 * STM32F4 bring-up examples use as their default, specifically because
 * it needs no over-drive, comfortable flash wait-state margin, and
 * clean division: VCO = 8MHz/8*336 = 336MHz, SYSCLK = 336/4 = 84MHz,
 * and PLLQ=7 gives 336/7 = exactly 48MHz - a correct USB/SDIO clock, for
 * whenever a later stage wants it. Pushing to 180MHz with over-drive is
 * a reasonable later exercise once the kernel exists and you have a way
 * to notice if it's misbehaving (a UART console, a running scheduler) -
 * doing it now, before you can debug anything, is needless risk for a
 * bring-up stage.
 *
 * ============================================================
 * The sequence, and why the order matters
 * ============================================================
 *   1. Enable HSE, wait HSERDY. On this board HSE isn't a crystal on the
 *      main MCU - it's an 8MHz clock signal fed in from the ST-LINK's
 *      own MCU (via the MCO pin, through solder bridges SB16/SB50 in
 *      their factory configuration). Same RCC sequence either way; the
 *      chip doesn't know or care where HSE physically comes from.
 *   2. Enable the PWR peripheral's clock and select voltage Scale 1 -
 *      the widest operating-frequency headroom of the available scales.
 *      This does NOT need a ready-poll; only over-drive mode does.
 *   3. Program PLLCFGR (source=HSE, M=8, N=336, P=/4, Q=7), enable the
 *      PLL, wait PLLRDY.
 *   4. Set flash latency (2 wait states at 84MHz) and enable the
 *      prefetch/instruction/data caches - must happen before switching
 *      SYSCLK to the PLL, same reasoning as the F103 version: flash
 *      can't be read fast enough at the new clock speed without this.
 *   5. Set AHB/APB1/APB2 prescalers.
 *   6. Switch SW to PLL, confirm via SWS.
 */
#include "stm32f446_regs.h"

void SystemInit(void)
{
    /* 1. HSE */
    RCC->CR |= RCC_CR_HSEON;
    while (!(RCC->CR & RCC_CR_HSERDY)) {
        /* wait */
    }

    /* 2. PWR clock + voltage scale */
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    PWR->CR |= PWR_CR_VOS_SCALE1;

    /* 3. PLL: HSE / 8 * 336 / 4 = 84MHz (PLLQ=7 -> 48MHz side output, unused for now) */
    RCC->PLLCFGR = RCC_PLLCFGR_PLLSRC_HSE
                  | RCC_PLLCFGR_PLLM(8)
                  | RCC_PLLCFGR_PLLN(336)
                  | RCC_PLLCFGR_PLLP_DIV4
                  | RCC_PLLCFGR_PLLQ(7);
    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY)) {
        /* wait */
    }

    /* 4. Flash: 2 wait states @ 84MHz/3.3V, caches + prefetch on */
    FLASH_ACR = FLASH_ACR_LATENCY_2WS | FLASH_ACR_PRFTEN | FLASH_ACR_ICEN | FLASH_ACR_DCEN;

    /* 5. Bus prescalers: HCLK=84MHz, PCLK1=42MHz (APB1 max), PCLK2=84MHz (APB2 max) */
    RCC->CFGR |= RCC_CFGR_HPRE_DIV1 | RCC_CFGR_PPRE1_DIV2 | RCC_CFGR_PPRE2_DIV1;

    /* 6. Switch SYSCLK to PLL, confirm */
    RCC->CFGR = (RCC->CFGR & ~0x3UL) | RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS_Msk) != RCC_CFGR_SWS_PLL) {
        /* wait */
    }
}

uint32_t SystemCoreClock = 84000000UL;
