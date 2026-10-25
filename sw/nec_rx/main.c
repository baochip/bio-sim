#include <stdint.h>

#include "bio.h" // this must always be first

// ASSUME: the core clock is external, on the receiver's pin, so every quantum
// is a RISING edge - the end of a burst. An interval is a gap plus the 560us
// burst that ends it: bit 0 = 1120us, bit 1 = 2250us, repeat = 2810us,
// leader = 5060us.
//
// The host pushes the pin mask then five interval boundaries in BIO clock
// ticks on FIFO0; sending them instead of baking them in keeps decode correct
// at any BIO clock. Whole 32-bit frames come back on FIFO0, one word each.

#define REPEAT_FRAME 0xFFFFFFFF // pushed when the remote repeats (button held)
#define TOP_BIT      0x80000000 // NEC sends LSB first, so bits shift in at the top
#define COUNTER_MAX  0x3FFFFFFF // x31's counter; the top two bits are the core ID

void main(void) {
    uint32_t pin_mask = pop_fifo0(); // blocks until the host sends it
    set_gpio_mask(pin_mask);
    set_input_pins(pin_mask);

    uint32_t bound_min = pop_fifo0();
    uint32_t bound_bit0 = pop_fifo0();
    uint32_t bound_bit1 = pop_fifo0();
    uint32_t bound_repeat = pop_fifo0();
    uint32_t bound_max = pop_fifo0();

    uint32_t prev = aclk_counter();
    uint32_t bits = 0;
    uint32_t count = 0;
    uint32_t assembling = 0;

    while (1) {
        wait_quantum();
        uint32_t now = aclk_counter();
        uint32_t interval = (now >= prev) ? (now - prev) : (COUNTER_MAX - prev) + now;
        prev = now;

        if (interval < bound_min || interval >= bound_max) {
            assembling = 0; // noise, or the idle gap between frames
        } else if (interval < bound_bit1) {
            if (assembling) { // data bit: 0 below the bit-0 boundary, 1 above
                bits >>= 1;
                if (interval >= bound_bit0) {
                    bits |= TOP_BIT;
                }
                if (++count == 32) {
                    push_fifo0(bits);
                    assembling = 0;
                }
            }
        } else if (interval < bound_repeat) {
            if (assembling) {
                assembling = 0;
            } else {
                push_fifo0(REPEAT_FRAME); // only meaningful between frames
            }
        } else {
            assembling = 1; // leader - start a fresh frame
            bits = 0;
            count = 0;
        }
    }
}
