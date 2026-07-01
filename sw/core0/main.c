#include <stdint.h>

#include "bio.h" // this must always be first

void main(void) {
    uint32_t tx = 0;
    uint32_t rx;
    while(1) {
        push_fifo1(tx);
        rx = pop_fifo0();
        tx = rx + 1;
    }
}
