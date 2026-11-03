#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "wcam_pacer.h"

int main(void)
{
    struct wcam_pacer pacer = {0, 0};
    /* Do not halve the frame rate if processing barely misses a deadline. */
    assert(wcam_pacer_delay(&pacer, 34, 1000) == 0);
    assert(wcam_pacer_delay(&pacer, 36, 1000) == 31);
    pacer.deadline = 0;
    pacer.fraction = 0;
    for (unsigned frame = 0; frame < 30; ++frame) {
        uint32_t start = pacer.deadline;
        uint32_t delay = wcam_pacer_delay(&pacer, start + 2, 1000);
        assert(delay == 31 || delay == 32);
    }
    assert(pacer.deadline == 1000);
    pacer.deadline = UINT32_MAX - 10;
    pacer.fraction = 0;
    assert(wcam_pacer_delay(&pacer, 4, 1000) == 18);
    puts("pacer: overrun, fractional frame periods and tick wrap passed");
}
