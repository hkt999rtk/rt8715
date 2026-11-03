#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wcam_pattern.h"

/* Catch wrong chroma order, plane offset, frame bounds and stale digits. */
int main(void)
{
    const size_t size = 1920u * 1080u * 3u / 2u;
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
        unsigned x = bar * 240 + 120;
        assert(frame[x] == y[bar]);
        assert(frame[1079u * 1920 + x] == y[bar]);
        assert(frame[1920u * 1080 + x] == uv[bar][0]);
        assert(frame[1920u * 1080 + x + 1] == uv[bar][1]);
    }
    memcpy(reference, frame, size);
    wcam_pattern_counter(frame, 0);
    assert(memcmp(reference, frame, size) != 0);
    assert(memcmp(reference, frame, 400u * 1920) == 0);
    memcpy(reference, frame, size);
    wcam_pattern_counter(frame, 1);
    assert(memcmp(reference, frame, size) != 0);
    wcam_pattern_counter(frame, UINT32_MAX);
    wcam_pattern_counter(frame, 0);
    assert(memcmp(reference, frame, size) == 0);
    for (unsigned i = 0; i < 32; ++i) {
        assert(allocation[i] == 0xa5);
        assert(allocation[32 + size + i] == 0xa5);
    }
    free(reference);
    free(allocation);
    puts("pattern: colors, counter updates, rollover and bounds passed");
    return 0;
}
