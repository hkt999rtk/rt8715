#ifndef CARPLAY_OTA_COMPAT_H
#define CARPLAY_OTA_COMPAT_H
#include <stdint.h>
typedef struct {
	char *host;
	int port;
	char *resource;
	int fd;
	uint8_t type;
	void *tls;
	void *otactrl;
	void *redirect;
	void *otaTargetHdr;
	void (*progress_cb)(int percent);
	uint8_t *firmware_buffer;
	uint32_t firmware_buffer_size;
} carplay_ota_context_t;
int ota_update_init(carplay_ota_context_t *, char *, int, char *, uint8_t);
int ota_update_start(carplay_ota_context_t *);
int ota_update_deinit(carplay_ota_context_t *);
int download_fw_program(carplay_ota_context_t *, const uint8_t *, uint32_t);
#endif
