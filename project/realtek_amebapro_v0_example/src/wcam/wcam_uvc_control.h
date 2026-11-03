#ifndef WCAM_UVC_CONTROL_H
#define WCAM_UVC_CONTROL_H

#include <stddef.h>
#include <stdint.h>

#define WCAM_FPS 30u
#define WCAM_BITRATE 2000000u
#define WCAM_FRAME_INTERVAL (10000000u / WCAM_FPS)
#define WCAM_ENCODED_SIZE (512u * 1024u)
#define WCAM_PAYLOAD_SIZE 1000u
#define WCAM_PROBE_SIZE 34u

struct wcam_uvc_control {
    uint8_t probe[WCAM_PROBE_SIZE];
    uint8_t commit[WCAM_PROBE_SIZE];
};

void wcam_uvc_control_init(struct wcam_uvc_control *control);
int wcam_uvc_control_get(const struct wcam_uvc_control *control,
                         unsigned request, unsigned selector,
                         uint8_t *reply, size_t capacity);
int wcam_uvc_control_set(struct wcam_uvc_control *control, unsigned selector,
                         const uint8_t *data, size_t length);
#endif
