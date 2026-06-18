#include <stdint.h>

#include "bio.h" // this must always be first

#define GPIO_PIN 21

void main(void) {
    uint32_t output_mask = 1 << GPIO_PIN;

    set_gpio_mask(output_mask);
    set_output_pins(output_mask);

    clear_gpio_pins_n(!output_mask); // drives it low
    while (1) {
        for(int i = 0; i < 10; i++) {
            set_gpio_pins(output_mask);
        }
        for(int i = 0; i < 10; i++) {
            clear_gpio_pins_n(!output_mask); // drives the pin low
        }
    }
}
