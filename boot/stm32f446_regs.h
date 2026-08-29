/*
 * Hand-written register definitions for the STM32F446RE (NUCLEO-F446RE).
 *
 * Same philosophy as before: no CMSIS/HAL, addresses taken straight from
 * RM0390 (STM32F446xx reference manual). If you previously read the F103
 * version of this file: notice the peripheral bus layout is different on
 * F4 parts. F1 hung almost everything off APB1/APB2; F4 introduces AHB1
 * as the bus GPIO lives on, and the PLL configuration - packed into CFGR
 * on F1 - gets its own dedicated register, PLLCFGR, here. Same underlying
 * idea (multiply a reference clock up to something fast), different bits.
 */
#ifndef STM32F446_REGS_H
#define STM32F446_REGS_H

#include <stdint.h>

#define __IO volatile

/* ---------------- RCC: Reset and Clock Control (base 0x40023800) ---------------- */
typedef struct {
    __IO uint32_t CR;          /* 0x00 HSION, HSERDY, HSEON, PLLON, PLLRDY... */
    __IO uint32_t PLLCFGR;     /* 0x04 PLL config: PLLM, PLLN, PLLP, PLLSRC, PLLQ - its own register on F4 */
    __IO uint32_t CFGR;        /* 0x08 SW, SWS, HPRE, PPRE1, PPRE2 - just prescalers + switch now */
    __IO uint32_t CIR;         /* 0x0C */
    __IO uint32_t AHB1RSTR;    /* 0x10 */
    __IO uint32_t AHB2RSTR;    /* 0x14 */
    __IO uint32_t AHB3RSTR;    /* 0x18 */
    uint32_t      RESERVED0;   /* 0x1C */
    __IO uint32_t APB1RSTR;    /* 0x20 */
    __IO uint32_t APB2RSTR;    /* 0x24 */
    uint32_t      RESERVED1[2];/* 0x28-0x2C */
    __IO uint32_t AHB1ENR;     /* 0x30 GPIOA..GPIOH clock enables live here now, not APB2 */
    __IO uint32_t AHB2ENR;     /* 0x34 */
    __IO uint32_t AHB3ENR;     /* 0x38 */
    uint32_t      RESERVED2;   /* 0x3C */
    __IO uint32_t APB1ENR;     /* 0x40 PWREN lives here (bit 28) */
    __IO uint32_t APB2ENR;     /* 0x44 */
} RCC_TypeDef;

#define RCC_BASE        0x40023800UL
#define RCC             ((RCC_TypeDef *)RCC_BASE)

#define RCC_CR_HSEON        (1UL << 16)
#define RCC_CR_HSERDY       (1UL << 17)
#define RCC_CR_PLLON        (1UL << 24)
#define RCC_CR_PLLRDY       (1UL << 25)

/* PLLCFGR fields. VCO_in = HSE / PLLM (aim for 1-2MHz to minimize jitter).
 * VCO_out = VCO_in * PLLN. SYSCLK = VCO_out / PLLP. USB/SDIO clock = VCO_out / PLLQ.
 * For NUCLEO-F446RE: HSE=8MHz, PLLM=8 -> VCOin=1MHz, PLLN=336 -> VCOout=336MHz,
 * PLLP=/4 -> SYSCLK=84MHz, PLLQ=7 -> 48MHz (usable as a USB clock later, unused now). */
#define RCC_PLLCFGR_PLLM(x)   ((x) & 0x3FUL)          /* bits [5:0] */
#define RCC_PLLCFGR_PLLN(x)   (((x) & 0x1FFUL) << 6)  /* bits [14:6] */
#define RCC_PLLCFGR_PLLP_DIV4 (0x1UL << 16)           /* bits [17:16]: 00=/2 01=/4 10=/6 11=/8 */
#define RCC_PLLCFGR_PLLSRC_HSE (1UL << 22)
#define RCC_PLLCFGR_PLLQ(x)   (((x) & 0xFUL) << 24)   /* bits [27:24] */

#define RCC_CFGR_SW_PLL       (0x2UL << 0)
#define RCC_CFGR_SWS_Msk      (0x3UL << 2)
#define RCC_CFGR_SWS_PLL      (0x2UL << 2)
#define RCC_CFGR_HPRE_DIV1    (0x0UL << 4)   /* AHB prescaler /1  -> HCLK = 84MHz */
#define RCC_CFGR_PPRE1_DIV2   (0x4UL << 10)  /* APB1 prescaler /2 -> PCLK1 = 42MHz (max for APB1) */
#define RCC_CFGR_PPRE2_DIV1   (0x0UL << 13)  /* APB2 prescaler /1 -> PCLK2 = 84MHz (max for APB2) */

#define RCC_AHB1ENR_GPIOAEN   (1UL << 0)
#define RCC_APB1ENR_PWREN     (1UL << 28)

/* ---------------- PWR: Power Control (base 0x40007000) ---------------- */
typedef struct {
    __IO uint32_t CR;   /* 0x00 VOS (voltage scaling), ODEN/ODSW (over-drive, not needed <=168MHz) */
    __IO uint32_t CSR;  /* 0x04 */
} PWR_TypeDef;

#define PWR_BASE        0x40007000UL
#define PWR             ((PWR_TypeDef *)PWR_BASE)
#define PWR_CR_VOS_SCALE1  (0x3UL << 14)  /* Scale 1: widest frequency headroom; safe default choice */

/* ---------------- FLASH interface (base 0x40023C00) ---------------- */
#define FLASH_ACR       (*(__IO uint32_t *)0x40023C00UL)
#define FLASH_ACR_LATENCY_2WS  0x2UL   /* 2 wait states: required for 60MHz < HCLK <= 90MHz @ 2.7-3.6V */
#define FLASH_ACR_PRFTEN       (1UL << 8)
#define FLASH_ACR_ICEN         (1UL << 9)
#define FLASH_ACR_DCEN         (1UL << 10)

/* ---------------- GPIO (F4 "new style": 2-bit MODER, not F1's 4-bit CRL/CRH) ---------------- */
typedef struct {
    __IO uint32_t MODER;    /* 0x00 2 bits/pin: 00=input 01=output 10=AF 11=analog */
    __IO uint32_t OTYPER;   /* 0x04 1 bit/pin: 0=push-pull 1=open-drain */
    __IO uint32_t OSPEEDR;  /* 0x08 2 bits/pin: low/medium/fast/high */
    __IO uint32_t PUPDR;    /* 0x0C 2 bits/pin: 00=none 01=pull-up 10=pull-down */
    __IO uint32_t IDR;      /* 0x10 */
    __IO uint32_t ODR;      /* 0x14 */
    __IO uint32_t BSRR;     /* 0x18 same set/reset-atomic convention as F1: low16=set, high16=reset */
    __IO uint32_t LCKR;     /* 0x1C */
    __IO uint32_t AFR[2];   /* 0x20/0x24 alternate function select, low pins/high pins */
} GPIO_TypeDef;

#define GPIOA_BASE      0x40020000UL
#define GPIOA           ((GPIO_TypeDef *)GPIOA_BASE)

#define GPIO_MODER_OUTPUT(pin)   (0x1UL << ((pin) * 2))
#define GPIO_MODER_MASK(pin)     (0x3UL << ((pin) * 2))

#endif /* STM32F446_REGS_H */
