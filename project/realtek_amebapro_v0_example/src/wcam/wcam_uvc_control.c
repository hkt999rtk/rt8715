#include "wcam_uvc_control.h"
#include <string.h>

static void put32(uint8_t *p, uint32_t value)
{
    unsigned i;
    for (i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8 * i));
}

static void fixed_control(uint8_t *p)
{
    memset(p, 0, WCAM_PROBE_SIZE);
    p[0] = 1; /* Fixed frame interval. */
    p[2] = p[3] = 1;
    put32(p + 4, WCAM_FRAME_INTERVAL);
    put32(p + 18, WCAM_ENCODED_SIZE);
    put32(p + 22, WCAM_PAYLOAD_SIZE);
    put32(p + 26, 48000000);
    p[30] = 3; /* FID and EOF framing. */
    p[31] = p[32] = p[33] = 1;
}

void wcam_uvc_control_init(struct wcam_uvc_control *control)
{
    fixed_control(control->probe);
    fixed_control(control->commit);
}

int wcam_uvc_control_get(const struct wcam_uvc_control *control,
                         unsigned request, unsigned selector,
                         uint8_t *reply, size_t capacity)
{
    uint8_t data[WCAM_PROBE_SIZE] = {0};
    size_t length = sizeof(data);
    if (selector != 1 && selector != 2) return -1;
    switch (request) {
    case 0x81: /* GET_CUR */
        memcpy(data, selector == 1 ? control->probe : control->commit, length);
        break;
    case 0x82: /* GET_MIN */
    case 0x83: /* GET_MAX: the only supported mode, never index -1. */
    case 0x87: /* GET_DEF */
        fixed_control(data);
        break;
    case 0x84: /* GET_RES: no adjustable fields. */
        break;
    case 0x85: /* GET_LEN */
        data[0] = WCAM_PROBE_SIZE;
        length = 2;
        break;
    case 0x86: /* GET_INFO: GET and SET supported. */
        data[0] = 3;
        length = 1;
        break;
    default:
        return -1;
    }
    if (length > capacity) length = capacity;
    memcpy(reply, data, length);
    return (int)length;
}

int wcam_uvc_control_set(struct wcam_uvc_control *control, unsigned selector,
                         const uint8_t *data, size_t length)
{
    if (!data || (length != 26 && length != WCAM_PROBE_SIZE) ||
        (selector != 1 && selector != 2)) return -1;
    /* UVC negotiation clamps all requests, including index 0/255, to
     * the sole advertised mode. No host-supplied value indexes a table. */
    fixed_control(selector == 1 ? control->probe : control->commit);
    return 0;
}
