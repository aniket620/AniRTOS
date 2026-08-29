/*
 * Tiny register-level GPIO driver - drives the Nucleo-F446RE's onboard
 * user LED (LD2, PA5).
 */
#ifndef GPIO_H
#define GPIO_H

void gpio_led_init(void);
void gpio_led_on(void);
void gpio_led_off(void);
void gpio_led_toggle(void);

#endif /* GPIO_H */
