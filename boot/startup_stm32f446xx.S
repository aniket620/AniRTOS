/*
 * Cortex-M4 vector table + reset entry point for STM32F446RE.
 *
 * The boot mechanics here are IDENTICAL to Cortex-M3 (see
 * docs/cortex_m_boot.md for the full explanation): the CPU reads word 0
 * as the initial MSP and word 1 as the reset entry point, unconditionally,
 * in hardware, before anything else runs. Cortex-M4 is a superset of the
 * M3 architecture (adds DSP instructions and an optional FPU) - it does
 * not change any of that boot sequence.
 *
 * What IS different from the F103 version of this file: the peripheral
 * IRQ list. STM32F446 has far more peripherals than the STM32F103 (USB
 * OTG, SAI, QUADSPI, a second I2C-FMP block, CAN2, DCMI...), so the
 * vector table is much longer - 86 external IRQs vs F103's 43.
 */

    .syntax unified
    .cpu cortex-m4
    .thumb

/* ------------------------------------------------------------------ */
/* Vector table                                                       */
/* ------------------------------------------------------------------ */
    .section .isr_vector, "a", %progbits
    .type g_pfnVectors, %object
    .size g_pfnVectors, .-g_pfnVectors

g_pfnVectors:
    .word _estack
    .word Reset_Handler
    .word NMI_Handler
    .word HardFault_Handler
    .word MemManage_Handler
    .word BusFault_Handler
    .word UsageFault_Handler
    .word 0
    .word 0
    .word 0
    .word 0
    .word SVC_Handler
    .word DebugMon_Handler
    .word 0
    .word PendSV_Handler
    .word SysTick_Handler

    /* --- STM32F446 peripheral IRQs (86 entries) --- */
    .word WWDG_IRQHandler
    .word PVD_IRQHandler
    .word TAMP_STAMP_IRQHandler
    .word RTC_WKUP_IRQHandler
    .word FLASH_IRQHandler
    .word RCC_IRQHandler
    .word EXTI0_IRQHandler
    .word EXTI1_IRQHandler
    .word EXTI2_IRQHandler
    .word EXTI3_IRQHandler
    .word EXTI4_IRQHandler
    .word DMA1_Stream0_IRQHandler
    .word DMA1_Stream1_IRQHandler
    .word DMA1_Stream2_IRQHandler
    .word DMA1_Stream3_IRQHandler
    .word DMA1_Stream4_IRQHandler
    .word DMA1_Stream5_IRQHandler
    .word DMA1_Stream6_IRQHandler
    .word ADC_IRQHandler
    .word CAN1_TX_IRQHandler
    .word CAN1_RX0_IRQHandler
    .word CAN1_RX1_IRQHandler
    .word CAN1_SCE_IRQHandler
    .word EXTI9_5_IRQHandler
    .word TIM1_BRK_TIM9_IRQHandler
    .word TIM1_UP_TIM10_IRQHandler
    .word TIM1_TRG_COM_TIM11_IRQHandler
    .word TIM1_CC_IRQHandler
    .word TIM2_IRQHandler
    .word TIM3_IRQHandler
    .word TIM4_IRQHandler
    .word I2C1_EV_IRQHandler
    .word I2C1_ER_IRQHandler
    .word I2C2_EV_IRQHandler
    .word I2C2_ER_IRQHandler
    .word SPI1_IRQHandler
    .word SPI2_IRQHandler
    .word USART1_IRQHandler
    .word USART2_IRQHandler
    .word USART3_IRQHandler
    .word EXTI15_10_IRQHandler
    .word RTC_Alarm_IRQHandler
    .word OTG_FS_WKUP_IRQHandler
    .word TIM8_BRK_TIM12_IRQHandler
    .word TIM8_UP_TIM13_IRQHandler
    .word TIM8_TRG_COM_TIM14_IRQHandler
    .word TIM8_CC_IRQHandler
    .word DMA1_Stream7_IRQHandler
    .word FMC_IRQHandler
    .word SDIO_IRQHandler
    .word TIM5_IRQHandler
    .word SPI3_IRQHandler
    .word UART4_IRQHandler
    .word UART5_IRQHandler
    .word TIM6_DAC_IRQHandler
    .word TIM7_IRQHandler
    .word DMA2_Stream0_IRQHandler
    .word DMA2_Stream1_IRQHandler
    .word DMA2_Stream2_IRQHandler
    .word DMA2_Stream3_IRQHandler
    .word DMA2_Stream4_IRQHandler
    .word CAN2_TX_IRQHandler
    .word CAN2_RX0_IRQHandler
    .word CAN2_RX1_IRQHandler
    .word CAN2_SCE_IRQHandler
    .word OTG_FS_IRQHandler
    .word DMA2_Stream5_IRQHandler
    .word DMA2_Stream6_IRQHandler
    .word DMA2_Stream7_IRQHandler
    .word USART6_IRQHandler
    .word I2C3_EV_IRQHandler
    .word I2C3_ER_IRQHandler
    .word OTG_HS_EP1_OUT_IRQHandler
    .word OTG_HS_EP1_IN_IRQHandler
    .word OTG_HS_WKUP_IRQHandler
    .word OTG_HS_IRQHandler
    .word DCMI_IRQHandler
    .word FPU_IRQHandler
    .word SPI4_IRQHandler
    .word SAI1_IRQHandler
    .word SAI2_IRQHandler
    .word QUADSPI_IRQHandler
    .word CEC_IRQHandler
    .word SPDIF_RX_IRQHandler
    .word FMPI2C1_Event_IRQHandler
    .word FMPI2C1_Error_IRQHandler

/* ------------------------------------------------------------------ */
/* Reset_Handler                                                       */
/* ------------------------------------------------------------------ */
    .section .text.Reset_Handler
    .weak Reset_Handler
    .type Reset_Handler, %function
Reset_Handler:
    /* Identical to the F103 version: copy .data FLASH->RAM, zero .bss,
     * then hand off to C. See docs/cortex_m_boot.md section 3 for why. */
    ldr r0, =_sdata
    ldr r1, =_edata
    ldr r2, =_etext
copy_data_loop:
    cmp r0, r1
    bge copy_data_done
    ldr r3, [r2], #4
    str r3, [r0], #4
    b copy_data_loop
copy_data_done:

    ldr r0, =_sbss
    ldr r1, =_ebss
    movs r2, #0
zero_bss_loop:
    cmp r0, r1
    bge zero_bss_done
    str r2, [r0], #4
    b zero_bss_loop
zero_bss_done:

    bl SystemInit       /* clock tree bring-up: HSE 8MHz -> PLL -> 84MHz (boot/system_stm32f4.c) */
    bl main

    b .

    .size Reset_Handler, .-Reset_Handler

/* ------------------------------------------------------------------ */
/* Default handlers                                                    */
/* ------------------------------------------------------------------ */
    .macro fault_stub name
    .weak \name
    .type \name, %function
\name:
    b .
    .size \name, .-\name
    .endm

    fault_stub NMI_Handler
    fault_stub HardFault_Handler
    fault_stub MemManage_Handler
    fault_stub BusFault_Handler
    fault_stub UsageFault_Handler
    fault_stub DebugMon_Handler

    .macro weak_return_stub name
    .weak \name
    .type \name, %function
\name:
    bx lr
    .size \name, .-\name
    .endm

    weak_return_stub SVC_Handler
    weak_return_stub PendSV_Handler
    weak_return_stub SysTick_Handler

    .weak Default_Handler
    .type Default_Handler, %function
Default_Handler:
    b .
    .size Default_Handler, .-Default_Handler

    .macro irq_alias name
    .weak \name
    .thumb_set \name, Default_Handler
    .endm

    irq_alias WWDG_IRQHandler
    irq_alias PVD_IRQHandler
    irq_alias TAMP_STAMP_IRQHandler
    irq_alias RTC_WKUP_IRQHandler
    irq_alias FLASH_IRQHandler
    irq_alias RCC_IRQHandler
    irq_alias EXTI0_IRQHandler
    irq_alias EXTI1_IRQHandler
    irq_alias EXTI2_IRQHandler
    irq_alias EXTI3_IRQHandler
    irq_alias EXTI4_IRQHandler
    irq_alias DMA1_Stream0_IRQHandler
    irq_alias DMA1_Stream1_IRQHandler
    irq_alias DMA1_Stream2_IRQHandler
    irq_alias DMA1_Stream3_IRQHandler
    irq_alias DMA1_Stream4_IRQHandler
    irq_alias DMA1_Stream5_IRQHandler
    irq_alias DMA1_Stream6_IRQHandler
    irq_alias ADC_IRQHandler
    irq_alias CAN1_TX_IRQHandler
    irq_alias CAN1_RX0_IRQHandler
    irq_alias CAN1_RX1_IRQHandler
    irq_alias CAN1_SCE_IRQHandler
    irq_alias EXTI9_5_IRQHandler
    irq_alias TIM1_BRK_TIM9_IRQHandler
    irq_alias TIM1_UP_TIM10_IRQHandler
    irq_alias TIM1_TRG_COM_TIM11_IRQHandler
    irq_alias TIM1_CC_IRQHandler
    irq_alias TIM2_IRQHandler
    irq_alias TIM3_IRQHandler
    irq_alias TIM4_IRQHandler
    irq_alias I2C1_EV_IRQHandler
    irq_alias I2C1_ER_IRQHandler
    irq_alias I2C2_EV_IRQHandler
    irq_alias I2C2_ER_IRQHandler
    irq_alias SPI1_IRQHandler
    irq_alias SPI2_IRQHandler
    irq_alias USART1_IRQHandler
    irq_alias USART2_IRQHandler
    irq_alias USART3_IRQHandler
    irq_alias EXTI15_10_IRQHandler
    irq_alias RTC_Alarm_IRQHandler
    irq_alias OTG_FS_WKUP_IRQHandler
    irq_alias TIM8_BRK_TIM12_IRQHandler
    irq_alias TIM8_UP_TIM13_IRQHandler
    irq_alias TIM8_TRG_COM_TIM14_IRQHandler
    irq_alias TIM8_CC_IRQHandler
    irq_alias DMA1_Stream7_IRQHandler
    irq_alias FMC_IRQHandler
    irq_alias SDIO_IRQHandler
    irq_alias TIM5_IRQHandler
    irq_alias SPI3_IRQHandler
    irq_alias UART4_IRQHandler
    irq_alias UART5_IRQHandler
    irq_alias TIM6_DAC_IRQHandler
    irq_alias TIM7_IRQHandler
    irq_alias DMA2_Stream0_IRQHandler
    irq_alias DMA2_Stream1_IRQHandler
    irq_alias DMA2_Stream2_IRQHandler
    irq_alias DMA2_Stream3_IRQHandler
    irq_alias DMA2_Stream4_IRQHandler
    irq_alias CAN2_TX_IRQHandler
    irq_alias CAN2_RX0_IRQHandler
    irq_alias CAN2_RX1_IRQHandler
    irq_alias CAN2_SCE_IRQHandler
    irq_alias OTG_FS_IRQHandler
    irq_alias DMA2_Stream5_IRQHandler
    irq_alias DMA2_Stream6_IRQHandler
    irq_alias DMA2_Stream7_IRQHandler
    irq_alias USART6_IRQHandler
    irq_alias I2C3_EV_IRQHandler
    irq_alias I2C3_ER_IRQHandler
    irq_alias OTG_HS_EP1_OUT_IRQHandler
    irq_alias OTG_HS_EP1_IN_IRQHandler
    irq_alias OTG_HS_WKUP_IRQHandler
    irq_alias OTG_HS_IRQHandler
    irq_alias DCMI_IRQHandler
    irq_alias FPU_IRQHandler
    irq_alias SPI4_IRQHandler
    irq_alias SAI1_IRQHandler
    irq_alias SAI2_IRQHandler
    irq_alias QUADSPI_IRQHandler
    irq_alias CEC_IRQHandler
    irq_alias SPDIF_RX_IRQHandler
    irq_alias FMPI2C1_Event_IRQHandler
    irq_alias FMPI2C1_Error_IRQHandler
