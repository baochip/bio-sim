#include <stdint.h>

#include "bio.h" // this must always be first

void main(void) {
    uint32_t rx;
    while(1) {
        rx = pop_fifo1();
        push_fifo0(rx + 1);
    }
}
