#ifndef CARBOX_AAC_DECODER_ROUTER_H
#define CARBOX_AAC_DECODER_ROUTER_H

#include <stdint.h>

void carbox_audio_decode_profile_record_backend(const char *backend,
	uint32_t elapsed_us, int failed);

#endif
