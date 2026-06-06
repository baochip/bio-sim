#include <stdint.h>

#include "bio.h" // this must always be first
#include "ws2812.h"

#define SIM                   // turn on simulation hacks
#define QUANTUM_NS     150    // assumed quantum interval in nanoseconds
#define QUANTUM_PER_MS 6670   // quantum to wait per millisecond delay

/* ------------------------------------------------------------------ */
/*  Configuration                                                     */
/* ------------------------------------------------------------------ */

#define MAX_LEDS        100

/* ------------------------------------------------------------------ */
/*  Internals                                                         */
/* ------------------------------------------------------------------ */
static uint32_t led_buf[MAX_LEDS];

void main(void) {
    uint32_t pin;
    uint32_t actual_leds;

    uint32_t index = 0;
    uint32_t incoming = 0;

    // set event mask to 0, so that we don't halt when the event register is read.
    // this allows us to poll the event register without blocking on it.
    set_event_mask(0);

    // FIFO1
    uint32_t fifo1_slot0_mask = FIFO_EVENT_MASK(1, 0);

    // blocks until these are configured
    pin = pop_fifo1();
    actual_leds = pop_fifo1();

    while (1) {
        while ((event_status() & fifo1_slot0_mask) != 0) {
            incoming = pop_fifo1();

            // extract index & force it to be in-range
            if ((incoming & 0x80000000) != 0) {
              index = (incoming >> 24) & 0x7f;
              index = index % (sizeof(led_buf) / sizeof(uint32_t));
              led_buf[index] = (uint32_t) (incoming & 0x00ffffff);
            }
        }

        ws2812c(pin, led_buf, actual_leds);

        #ifndef SIM
        // decompose this into an outer loop that counts # of milliseconds
        // and then an inner loop that counts milliseconds so we have a roughly
        // "real time" sense of time in time_ms (in reality runs a bit slow
        // because we don't trim out the bitbang & next compute times)
        for (uint32_t j = 0; j < 35; j++) {
            for (uint32_t i = 0; i < QUANTUM_PER_MS / 10; i++) {
                // unroll this loop to mitigate timing offset in wfi mode
                wait_quantum();
                wait_quantum();
                wait_quantum();
                wait_quantum();
                wait_quantum();
                wait_quantum();
                wait_quantum();
                wait_quantum();
                wait_quantum();
                wait_quantum();
            }
            time_ms += 1;
        }
        #else
        // shorter quantum wait so we're not running out the simulation so long
        for (uint32_t i = 0; i < 20; i++) {
            wait_quantum();
        }
        #endif
    }
}
