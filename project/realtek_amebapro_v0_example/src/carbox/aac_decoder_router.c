#include "aac_decoder_router.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aacdec.h"

#ifdef UCHAR
#undef UCHAR
#endif
#ifdef USHORT
#undef USHORT
#endif
#ifdef UINT
#undef UINT
#endif
#ifdef ULONG
#undef ULONG
#endif
#include "aacdecoder_lib.h"
#include "hal_timer.h"

#ifndef CARBOX_AAC_DECODER_MODE
#define CARBOX_AAC_DECODER_MODE 0
#endif

#ifndef AAC_DECODER_ROUTE_PROFILE_WINDOW_MS
#define AAC_DECODER_ROUTE_PROFILE_WINDOW_MS 10000U
#endif

#ifndef CONFIG_AAC_DECODER_ROUTE_PROFILE
#define CONFIG_AAC_DECODER_ROUTE_PROFILE 0
#endif

#define CARBOX_AAC_DECODER_FDK_ONLY 0
#define CARBOX_AAC_DECODER_HELIX_AUTO 1

#if CARBOX_AAC_DECODER_MODE != CARBOX_AAC_DECODER_FDK_ONLY && \
	CARBOX_AAC_DECODER_MODE != CARBOX_AAC_DECODER_HELIX_AUTO
#error "CARBOX_AAC_DECODER_MODE must be 0 (FDK) or 1 (Helix AAC-LC auto)"
#endif

#define AAC_ROUTER_MAGIC 0x41414352U
#define AAC_ROUTER_INPUT_CAPACITY 8192U

typedef enum aac_router_backend_e {
	AAC_ROUTER_BACKEND_FDK = 0,
	AAC_ROUTER_BACKEND_HELIX = 1
} aac_router_backend_t;

typedef struct aac_router_s {
	uint32_t magic;
	HANDLE_AACDECODER fdk;
	HAACDecoder helix;
	aac_router_backend_t backend;
	TRANSPORT_TYPE transport;
	uint32_t layers;
	uint32_t sample_rate;
	uint32_t channels;
	uint32_t pending_bytes;
	uint32_t profile_start_us;
	uint32_t profile_helix_frames;
	uint32_t profile_fdk_frames;
	uint32_t profile_errors;
	uint32_t fallback_total;
	uint8_t pending[AAC_ROUTER_INPUT_CAPACITY];
} aac_router_t;

extern HANDLE_AACDECODER __real_aacDecoder_Open(TRANSPORT_TYPE transport,
	UINT layers);
extern AAC_DECODER_ERROR __real_aacDecoder_ConfigRaw(HANDLE_AACDECODER decoder,
	UCHAR *config[], const UINT length[]);
extern AAC_DECODER_ERROR __real_aacDecoder_Fill(HANDLE_AACDECODER decoder,
	UCHAR *buffer[], const UINT size[], UINT *valid);
extern AAC_DECODER_ERROR __real_aacDecoder_DecodeFrame(HANDLE_AACDECODER decoder,
	INT_PCM *output, const INT output_samples, const UINT flags);
extern AAC_DECODER_ERROR __real_aacDecoder_SetParam(
	const HANDLE_AACDECODER decoder, const AACDEC_PARAM parameter,
	const INT value);
extern void __real_aacDecoder_Close(HANDLE_AACDECODER decoder);

static aac_router_t *aac_router_from_handle(HANDLE_AACDECODER decoder)
{
	aac_router_t *router = (aac_router_t *)decoder;

	return router != NULL && router->magic == AAC_ROUTER_MAGIC ? router : NULL;
}

static uint32_t aac_router_read_bits(const uint8_t *data, uint32_t bit_offset,
	uint32_t count)
{
	uint32_t value = 0U;
	uint32_t bit;

	for (bit = 0U; bit < count; bit++) {
		uint32_t pos = bit_offset + bit;
		value = (value << 1) |
			((uint32_t)(data[pos >> 3] >> (7U - (pos & 7U))) & 1U);
	}
	return value;
}

static int aac_router_parse_lc_asc(const uint8_t *config, uint32_t length,
	uint32_t *sample_rate, uint32_t *channels)
{
	static const uint32_t rates[] = {
		96000U, 88200U, 64000U, 48000U, 44100U, 32000U, 24000U,
		22050U, 16000U, 12000U, 11025U, 8000U, 7350U
	};
	uint32_t object_type;
	uint32_t rate_index;
	uint32_t channel_config;
	uint32_t bit_offset = 0U;

	if (config == NULL || length < 2U) {
		return 0;
	}
	object_type = aac_router_read_bits(config, bit_offset, 5U);
	bit_offset += 5U;
	if (object_type == 31U) {
		if (length * 8U < bit_offset + 6U) {
			return 0;
		}
		object_type = 32U + aac_router_read_bits(config, bit_offset, 6U);
		bit_offset += 6U;
	}
	if (object_type != 2U || length * 8U < bit_offset + 8U) {
		return 0;
	}
	rate_index = aac_router_read_bits(config, bit_offset, 4U);
	bit_offset += 4U;
	if (rate_index == 15U || rate_index >= sizeof(rates) / sizeof(rates[0])) {
		return 0;
	}
	channel_config = aac_router_read_bits(config, bit_offset, 4U);
	bit_offset += 4U;
	if ((channel_config != 1U && channel_config != 2U) ||
	    length * 8U < bit_offset + 1U) {
		return 0;
	}
	/* Helix emits 1024-sample AAC-LC frames; reject the 960-sample form. */
	if (aac_router_read_bits(config, bit_offset, 1U) != 0U) {
		return 0;
	}
	*sample_rate = rates[rate_index];
	*channels = channel_config;
	return 1;
}

static void aac_router_select_fdk(aac_router_t *router, const char *reason)
{
	if (router->helix != NULL) {
		AACDeInitDecoder(router->helix);
		router->helix = NULL;
	}
	router->backend = AAC_ROUTER_BACKEND_FDK;
	printf("[AACROUTE] backend=FDK reason=%s\n", reason);
}

static void aac_router_profile_record(aac_router_t *router,
	const char *backend, AAC_DECODER_ERROR error)
{
#if CONFIG_AAC_DECODER_ROUTE_PROFILE
	uint32_t now_us = hal_read_curtime_us();
	uint32_t window_us;

	if (router->profile_start_us == 0U) {
		router->profile_start_us = now_us;
	}
	if (strcmp(backend, "HELIX") == 0) {
		router->profile_helix_frames++;
	} else {
		router->profile_fdk_frames++;
	}
	if (error != AAC_DEC_OK) {
		router->profile_errors++;
	}
	window_us = now_us - router->profile_start_us;
	if (window_us < AAC_DECODER_ROUTE_PROFILE_WINDOW_MS * 1000U) {
		return;
	}
	printf("[AACROUTE] window_ms=%lu backend=%s frames helix/fdk/errors="
		"%lu/%lu/%lu fallback_total=%lu fmt=%lu/%lu pending=%luB\n",
		(unsigned long)(window_us / 1000U),
		router->backend == AAC_ROUTER_BACKEND_HELIX ? "HELIX" : "FDK",
		(unsigned long)router->profile_helix_frames,
		(unsigned long)router->profile_fdk_frames,
		(unsigned long)router->profile_errors,
		(unsigned long)router->fallback_total,
		(unsigned long)router->sample_rate,
		(unsigned long)router->channels,
		(unsigned long)router->pending_bytes);
	router->profile_start_us = now_us;
	router->profile_helix_frames = 0U;
	router->profile_fdk_frames = 0U;
	router->profile_errors = 0U;
#else
	(void)router;
	(void)backend;
	(void)error;
#endif
}

HANDLE_AACDECODER __wrap_aacDecoder_Open(TRANSPORT_TYPE transport, UINT layers)
{
#if CARBOX_AAC_DECODER_MODE == CARBOX_AAC_DECODER_FDK_ONLY
	return __real_aacDecoder_Open(transport, layers);
#else
	aac_router_t *router = calloc(1U, sizeof(*router));

	if (router == NULL) {
		return NULL;
	}
	router->fdk = __real_aacDecoder_Open(transport, layers);
	if (router->fdk == NULL) {
		free(router);
		return NULL;
	}
	router->magic = AAC_ROUTER_MAGIC;
	router->backend = AAC_ROUTER_BACKEND_FDK;
	router->transport = transport;
	router->layers = layers;
	return (HANDLE_AACDECODER)router;
#endif
}

AAC_DECODER_ERROR __wrap_aacDecoder_ConfigRaw(HANDLE_AACDECODER decoder,
	UCHAR *config[], const UINT length[])
{
#if CARBOX_AAC_DECODER_MODE == CARBOX_AAC_DECODER_FDK_ONLY
	return __real_aacDecoder_ConfigRaw(decoder, config, length);
#else
	aac_router_t *router = aac_router_from_handle(decoder);
	AAC_DECODER_ERROR error;
	uint32_t sample_rate;
	uint32_t channels;
	AACFrameInfo info;

	if (router == NULL) {
		return __real_aacDecoder_ConfigRaw(decoder, config, length);
	}
	error = __real_aacDecoder_ConfigRaw(router->fdk, config, length);
	if (error != AAC_DEC_OK) {
		return error;
	}
	router->pending_bytes = 0U;
	router->backend = AAC_ROUTER_BACKEND_FDK;
	if (router->helix != NULL) {
		AACDeInitDecoder(router->helix);
		router->helix = NULL;
	}
	if (router->transport != TT_MP4_RAW || router->layers != 1U ||
	    config == NULL || length == NULL ||
	    !aac_router_parse_lc_asc(config[0], length[0], &sample_rate,
		&channels)) {
		aac_router_select_fdk(router, "unsupported-config");
		return AAC_DEC_OK;
	}
	router->helix = AACInitDecoder();
	if (router->helix == NULL) {
		aac_router_select_fdk(router, "helix-init");
		return AAC_DEC_OK;
	}
	memset(&info, 0, sizeof(info));
	info.profile = AAC_PROFILE_LC;
	info.sampRateCore = (int)sample_rate;
	info.sampRateOut = (int)sample_rate;
	info.nChans = (int)channels;
	if (AACSetRawBlockParams(router->helix, 0, &info) != ERR_AAC_NONE) {
		aac_router_select_fdk(router, "helix-config");
		return AAC_DEC_OK;
	}
	router->sample_rate = sample_rate;
	router->channels = channels;
	router->backend = AAC_ROUTER_BACKEND_HELIX;
	printf("[AACROUTE] backend=HELIX profile=LC rate=%lu channels=%lu\n",
		(unsigned long)sample_rate, (unsigned long)channels);
	return AAC_DEC_OK;
#endif
}

AAC_DECODER_ERROR __wrap_aacDecoder_Fill(HANDLE_AACDECODER decoder,
	UCHAR *buffer[], const UINT size[], UINT *valid)
{
#if CARBOX_AAC_DECODER_MODE == CARBOX_AAC_DECODER_FDK_ONLY
	return __real_aacDecoder_Fill(decoder, buffer, size, valid);
#else
	aac_router_t *router = aac_router_from_handle(decoder);
	uint32_t bytes;
	uint32_t offset;

	if (router == NULL) {
		return __real_aacDecoder_Fill(decoder, buffer, size, valid);
	}
	if (router->backend == AAC_ROUTER_BACKEND_FDK) {
		return __real_aacDecoder_Fill(router->fdk, buffer, size, valid);
	}
	if (buffer == NULL || buffer[0] == NULL || size == NULL || valid == NULL ||
	    valid[0] > size[0]) {
		return AAC_DEC_INVALID_HANDLE;
	}
	bytes = valid[0];
	if (bytes > AAC_ROUTER_INPUT_CAPACITY - router->pending_bytes) {
		return AAC_DEC_NOT_ENOUGH_BITS;
	}
	offset = size[0] - bytes;
	memcpy(router->pending + router->pending_bytes, buffer[0] + offset, bytes);
	router->pending_bytes += bytes;
	valid[0] = 0U;
	return AAC_DEC_OK;
#endif
}

#if CARBOX_AAC_DECODER_MODE == CARBOX_AAC_DECODER_HELIX_AUTO
static AAC_DECODER_ERROR aac_router_fallback_decode(aac_router_t *router,
	INT_PCM *output, INT output_samples, UINT flags, const char *reason)
{
	UCHAR *input = router->pending;
	UINT size = router->pending_bytes;
	UINT valid = router->pending_bytes;
	AAC_DECODER_ERROR error;

	aac_router_select_fdk(router, reason);
	router->fallback_total++;
	router->pending_bytes = 0U;
	if (valid != 0U) {
		error = __real_aacDecoder_Fill(router->fdk, &input, &size, &valid);
		if (error != AAC_DEC_OK || valid != 0U) {
			return error != AAC_DEC_OK ? error : AAC_DEC_NOT_ENOUGH_BITS;
		}
	}
	return __real_aacDecoder_DecodeFrame(router->fdk, output, output_samples,
		flags);
}
#endif

AAC_DECODER_ERROR __wrap_aacDecoder_DecodeFrame(HANDLE_AACDECODER decoder,
	INT_PCM *output, const INT output_samples, const UINT flags)
{
#if CARBOX_AAC_DECODER_MODE == CARBOX_AAC_DECODER_FDK_ONLY
	AAC_DECODER_ERROR error;
	uint32_t start_us = hal_read_curtime_us();

	error = __real_aacDecoder_DecodeFrame(decoder, output, output_samples,
		flags);
	carbox_audio_decode_profile_record_backend("FDK",
		hal_read_curtime_us() - start_us, error != AAC_DEC_OK);
	return error;
#else
	aac_router_t *router = aac_router_from_handle(decoder);
	AAC_DECODER_ERROR error;
	uint32_t start_us = hal_read_curtime_us();
	const char *profile_name = "FDK";

	if (router == NULL) {
		error = __real_aacDecoder_DecodeFrame(decoder, output, output_samples,
			flags);
		goto out;
	}
	if (router->backend == AAC_ROUTER_BACKEND_FDK) {
		error = __real_aacDecoder_DecodeFrame(router->fdk, output,
			output_samples, flags);
		goto out;
	}
	profile_name = "HELIX";
	if ((flags & AACDEC_CONCEAL) != 0U) {
		error = aac_router_fallback_decode(router, output, output_samples,
			flags, "conceal-request");
		profile_name = "FDK-FALLBACK";
		goto out;
	}
	if (flags != 0U) {
		router->pending_bytes = 0U;
		error = AACFlushCodec(router->helix) == ERR_AAC_NONE ?
			AAC_DEC_OK : AAC_DEC_UNKNOWN;
		goto out;
	}
	if (router->pending_bytes == 0U) {
		error = AAC_DEC_NOT_ENOUGH_BITS;
		goto out;
	}
	if (output == NULL || output_samples < (INT)(AAC_MAX_NSAMPS *
		(int)router->channels)) {
		error = AAC_DEC_OUTPUT_BUFFER_TOO_SMALL;
		goto out;
	}
	{
		unsigned char *input = router->pending;
		int bytes_left = (int)router->pending_bytes;
		AACFrameInfo info;
		int helix_error = AACDecode(router->helix, &input, &bytes_left,
			(short *)output);

		memset(&info, 0, sizeof(info));
		if (helix_error == ERR_AAC_NONE) {
			AACGetLastFrameInfo(router->helix, &info);
		}
		if (helix_error == ERR_AAC_NONE && bytes_left == 0 &&
		    info.profile == AAC_PROFILE_LC &&
		    info.sampRateOut == (int)router->sample_rate &&
		    info.nChans == (int)router->channels &&
		    info.bitsPerSample == 16 &&
		    info.outputSamps == (int)(AAC_MAX_NSAMPS * router->channels)) {
			router->pending_bytes = 0U;
			error = AAC_DEC_OK;
			goto out;
		}
	}
	error = aac_router_fallback_decode(router, output, output_samples, flags,
		"helix-decode");
	profile_name = "FDK-FALLBACK";
out:
	carbox_audio_decode_profile_record_backend(profile_name,
		hal_read_curtime_us() - start_us, error != AAC_DEC_OK);
	if (router != NULL) {
		aac_router_profile_record(router, profile_name, error);
	}
	return error;
#endif
}

AAC_DECODER_ERROR __wrap_aacDecoder_SetParam(const HANDLE_AACDECODER decoder,
	const AACDEC_PARAM parameter, const INT value)
{
#if CARBOX_AAC_DECODER_MODE == CARBOX_AAC_DECODER_FDK_ONLY
	return __real_aacDecoder_SetParam(decoder, parameter, value);
#else
	aac_router_t *router = aac_router_from_handle(decoder);

	if (router == NULL) {
		return __real_aacDecoder_SetParam(decoder, parameter, value);
	}
	if (router->backend == AAC_ROUTER_BACKEND_HELIX &&
	    parameter == AAC_TPDEC_CLEAR_BUFFER) {
		router->pending_bytes = 0U;
		return AACFlushCodec(router->helix) == ERR_AAC_NONE ?
			AAC_DEC_OK : AAC_DEC_UNKNOWN;
	}
	if (router->backend == AAC_ROUTER_BACKEND_HELIX) {
		aac_router_select_fdk(router, "unsupported-param");
	}
	return __real_aacDecoder_SetParam(router->fdk, parameter, value);
#endif
}

void __wrap_aacDecoder_Close(HANDLE_AACDECODER decoder)
{
#if CARBOX_AAC_DECODER_MODE == CARBOX_AAC_DECODER_FDK_ONLY
	__real_aacDecoder_Close(decoder);
#else
	aac_router_t *router = aac_router_from_handle(decoder);

	if (router == NULL) {
		__real_aacDecoder_Close(decoder);
		return;
	}
	router->magic = 0U;
	if (router->helix != NULL) {
		AACDeInitDecoder(router->helix);
	}
	__real_aacDecoder_Close(router->fdk);
	free(router);
#endif
}
