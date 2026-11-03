#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "wcam_uvc_control.h"

int main(void)
{
    struct wcam_uvc_control control;
    uint8_t reply[40], expected[34], input[34];
    wcam_uvc_control_init(&control);
    assert(wcam_uvc_control_get(&control, 0x87, 1, expected, 34) == 34);
    assert(expected[2] == 1 && expected[3] == 1);
    /* 333333 in 100 ns units, 512 KiB frame ceiling, 1000-byte payload. */
    assert(!memcmp(expected + 4, "\x15\x16\x05\x00", 4));
    assert(!memcmp(expected + 18, "\x00\x00\x08\x00", 4));
    assert(!memcmp(expected + 22, "\xe8\x03\x00\x00", 4));
    for (unsigned request = 0x81; request <= 0x83; ++request) {
        assert(wcam_uvc_control_get(&control, request, 1, reply, 34) == 34);
        assert(!memcmp(reply, expected, 34));
    }
    memset(input, 0xff, sizeof(input));
    assert(wcam_uvc_control_set(&control, 1, input, 34) == 0);
    assert(wcam_uvc_control_get(&control, 0x81, 1, reply, 34) == 34);
    assert(!memcmp(reply, expected, 34));
    assert(wcam_uvc_control_set(&control, 2, input, 26) == 0);
    assert(wcam_uvc_control_get(&control, 0x81, 2, reply, 34) == 34);
    assert(!memcmp(reply, expected, 34));
    assert(wcam_uvc_control_set(&control, 2, input, 25) < 0);
    assert(wcam_uvc_control_set(&control, 3, input, 34) < 0);
    memset(reply, 0xa5, sizeof(reply));
    assert(wcam_uvc_control_get(&control, 0x85, 1, reply, 2) == 2);
    assert(reply[0] == 34 && reply[1] == 0 && reply[2] == 0xa5);
    assert(wcam_uvc_control_get(&control, 0x86, 1, reply, 1) == 1);
    assert(reply[0] == 3);
    assert(wcam_uvc_control_get(&control, 0x84, 1, reply, 34) == 34);
    for (unsigned i = 0; i < 34; ++i) assert(reply[i] == 0);
    assert(wcam_uvc_control_get(&control, 0x81, 1, reply, 26) == 26);
    assert(wcam_uvc_control_get(&control, 0x81, 0, reply, 34) < 0);
    assert(wcam_uvc_control_get(&control, 0xff, 1, reply, 34) < 0);
    puts("control: probe/commit, GET_MAX, malformed inputs and short replies passed");
}
