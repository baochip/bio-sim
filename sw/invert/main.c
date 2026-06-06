#include <stdint.h>

#include "bio.h" // this must always be first

#define GPIO_OUT 1
#define GPIO_IN 0

void main(void) {
    uint32_t output_mask = 1 << GPIO_OUT;
    uint32_t input_mask = 1 << GPIO_IN;

    set_gpio_mask(output_mask | input_mask);
    set_output_pins(output_mask);
    set_input_pins(input_mask);

    while (1) {
        uint32_t state = read_gpio_pins() & input_mask;
        if (state) {
            clear_gpio_pins_n(!output_mask);
        } else {
            set_gpio_pins(output_mask);
        }
    }
}
