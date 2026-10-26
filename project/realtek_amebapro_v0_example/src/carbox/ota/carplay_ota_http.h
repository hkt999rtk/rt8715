#ifndef CARBOX_OTA_HTTP_H
#define CARBOX_OTA_HTTP_H
#include <stddef.h>
#include <stdint.h>
int carbox_ota_tcp_connect(const char *host, int port);
int carbox_ota_send_all(int fd, const void *data, size_t len);
int carbox_ota_http_get(const char *host, int port, const char *path,
    int (*begin)(void *, uint32_t), int (*data)(void *, const uint8_t *, uint32_t), void *ctx);
#endif
