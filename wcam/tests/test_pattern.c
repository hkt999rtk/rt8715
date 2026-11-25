#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wcam_pattern.h"

/* Catch wrong chroma order, plane offset, frame bounds and stale digits. */
int main(void)
{
    const size_t size = 1280u * 720u * 3u / 2u;
    uint8_t *allocation = malloc(size + 64);
    uint8_t *reference = malloc(size);
    uint8_t *frame = allocation + 32;
    const uint8_t y[] = {235, 210, 170, 145, 106, 81, 41, 16};
    const uint8_t uv[][2] = {
        {128,128}, {16,146}, {166,16}, {54,34},
        {202,222}, {90,240}, {240,110}, {128,128}
    };
    assert(allocation && reference);
    memset(allocation, 0xa5, size + 64);
    wcam_pattern_init(frame);
    for (unsigned bar = 0; bar < 8; ++bar) {
        unsigned x = bar * 160 + 80;
        assert(frame[x] == y[bar]);
        assert(frame[719u * 1280 + x] == y[bar]);
        assert(frame[1280u * 720 + x] == uv[bar][0]);
        assert(frame[1280u * 720 + x + 1] == uv[bar][1]);
    }
    memcpy(reference, frame, size);
    wcam_pattern_counter(frame, 0);
    assert(memcmp(reference, frame, size) != 0);
    assert(memcmp(reference, frame, 300u * 1280) == 0);
    memcpy(reference, frame, size);
    wcam_pattern_counter(frame, 1);
    assert(memcmp(reference, frame, size) != 0);
    wcam_pattern_counter(frame, UINT32_MAX);
    wcam_pattern_counter(frame, 0);
    assert(memcmp(reference, frame, size) == 0);
    wcam_pattern_init_role(frame, WCAM_PATTERN_TX);
    assert(frame[80] == 16 && frame[719u*1280+80] == 16);
    wcam_pattern_counter_role(frame, 7, WCAM_PATTERN_TX);
    /* T has a lit top-right pixel, whereas R does not. */
    assert(frame[332u*1280+216] == 235);
    wcam_pattern_init_role(frame, WCAM_PATTERN_RX);
    for (size_t i = 0; i < WCAM_Y_SIZE; ++i) assert(frame[i] == 16);
    for (size_t i = WCAM_Y_SIZE; i < size; ++i) assert(frame[i] == 128);
    wcam_pattern_counter_role(frame, 7, WCAM_PATTERN_RX);
    /* The waiting message is centered on black; the bottom line changes. */
    assert(frame[295u*1280+244] == 235);
    memcpy(reference, frame, size);
    wcam_pattern_counter_role(frame, 8, WCAM_PATTERN_RX);
    assert(memcmp(reference, frame, size) != 0);
    wcam_pattern_counter_role(frame, 7, WCAM_PATTERN_RX);
    assert(memcmp(reference, frame, size) == 0);
    assert(frame[0] == 16 && frame[WCAM_Y_SIZE-1] == 16);
    for (unsigned i = 0; i < 32; ++i) {
        assert(allocation[i] == 0xa5);
        assert(allocation[32 + size + i] == 0xa5);
    }
    free(reference);
    free(allocation);
    puts("pattern: colors, counter updates, rollover and bounds passed");
    return 0;
}
