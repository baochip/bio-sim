#include <stdint.h>

#include "bio.h" // this must always be first

// ASSUME: quantum is set to (3.072 MHz) 6.144MHz

#define WS_PIN 1
#define SCK_PIN 2
#define SD_PIN 3

void main(void) {
    uint32_t ws_mask = 1 << WS_PIN;
    uint32_t sck_mask = 1 << SCK_PIN;
    uint32_t sd_mask = 1 << SD_PIN;

    set_gpio_mask(ws_mask | sck_mask | sd_mask);
    set_output_pins(ws_mask | sck_mask);
    set_input_pins(sd_mask);

    // signals start at 1
    set_gpio_pins(ws_mask | sck_mask);

    int cycle_count = 0;
    uint32_t sample = 0;
    while(1) {
        wait_quantum();
        // handle ws
        if (cycle_count == 0) {
            clear_gpio_pins_n(~ws_mask);
        }
        if (cycle_count == 1) {
            sample = 0;
        }
        if (cycle_count == 32) {
            set_gpio_pins(ws_mask);
        }
        clear_gpio_pins_n(~sck_mask);

        wait_quantum();
        if (read_gpio_pins() & sd_mask) {
            sample |= 1;
        }
        set_gpio_pins(sck_mask);
        if (cycle_count == 24) {
            push_fifo0(sample);
        }
        sample <<= 1;
        cycle_count ++;
        cycle_count %= 64;
    }
}
