#include <stdint.h>

#include "bio.h" // this must always be first

#define SAO_GPIO1 21
#define SAO_GPIO2 22
#define SAO_GPIO3 30
#define SAO_GPIO4 31

#define QUANTUM_PER_MS 1000 // assumes `--quantum 1MHz` passed as part of init
// gpio config assumes `--sao 1,3`

#define FIFO3_EMPTY_MASK FIFO_EVENT_MASK(3, 0)
#define FIFO3_AVAILABLE_MASK FIFO_EVENT_MASK(3, 1)

/* --- Tunables ---------------------------------------------------------- */
#define DEPTH               32
#define TOUCH_MARGIN        2
#define DEBOUNCE_COUNT      100
/* ----------------------------------------------------------------------- */

void main(void) {
    uint32_t output_mask = 1 << SAO_GPIO1;

    set_gpio_mask(output_mask);
    set_output_pins(output_mask);

    clear_gpio_pins_n(!output_mask); // drives it low
    while (1) {
        for(int i = 0; i < 2000; i++) {
            set_gpio_pins(output_mask);
        }
        for(int i = 0; i < 2000; i++) {
            clear_gpio_pins_n(!output_mask); // drives the pin low
        }
    }
}
