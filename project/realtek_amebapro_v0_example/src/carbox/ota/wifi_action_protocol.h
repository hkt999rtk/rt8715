#ifndef CARBOX_WIFI_ACTION_PROTOCOL_H
#define CARBOX_WIFI_ACTION_PROTOCOL_H
#include <stddef.h>
#include <stdint.h>
/* Max raw management frame accepted by this application. */
#define ACTION_FRAME_MAX 2304
struct action_ap { char ssid[33], password[65]; int secured; };
struct action_request {
    unsigned ap_count;
    unsigned parse_offset, parse_type, parse_length;
    char url[256], version[256];
    struct action_ap ap[];
};
struct action_url { char host[128], path[256]; int port; };
/* Allocate this many bytes for an output request before parsing frame_len bytes. */
size_t action_request_bytes(size_t frame_len);
/* 1 = recognized and valid, 0 = unrelated, -1 = malformed. */
int action_parse_frame(const uint8_t *p, size_t n, struct action_request *r);
int action_parse_url(const char *url, struct action_url *out);
#endif
