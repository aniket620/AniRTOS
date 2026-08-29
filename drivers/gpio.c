/*
 * NUCLEO-F446RE onboard LED = LD2 = PA5, wired ACTIVE-HIGH (opposite
 * polarity from the Blue Pill's PC13 LED - driving PA5 high turns this
 * one ON). Board-specific, not chip-specific - don't assume it elsewhere.
 */
#include "gpio.h"
#include "stm32f446_regs.h"

#define LED_PIN   5

void gpio_led_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;

    /* MODER: 2 bits/pin instead of F1's 4-bit CRL/CRH field. Clear PA5's
     * field then set it to "01" = general purpose output. OTYPER default
     * (0 = push-pull) and OSPEEDR/PUPDR defaults are already fine for a
     * simple LED, so we only need to touch MODER here. */
    uint32_t moder = GPIOA->MODER;
    moder &= ~GPIO_MODER_MASK(LED_PIN);
    moder |=  GPIO_MODER_OUTPUT(LED_PIN);
    GPIOA->MODER = moder;

    gpio_led_off();  /* start deterministic: LED off */
}

void gpio_led_on(void)
{
    /* Active-high: BSRR low half sets the pin high -> LED on. */
    GPIOA->BSRR = (1UL << LED_PIN);
}

void gpio_led_off(void)
{
    /* BSRR high half (bit + 16) atomically resets the pin -> LED off. */
    GPIOA->BSRR = (1UL << (LED_PIN + 16));
}

void gpio_led_toggle(void)
{
    if (GPIOA->ODR & (1UL << LED_PIN)) {
        gpio_led_off();
    } else {
        gpio_led_on();
    }
}
