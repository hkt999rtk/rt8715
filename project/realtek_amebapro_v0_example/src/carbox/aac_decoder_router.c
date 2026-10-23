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
#include "FreeRTOS.h"
#include "task.h"

#ifndef CARBOX_AAC_DECODER_MODE
#define CARBOX_AAC_DECODER_MODE 0
#endif

#ifndef AAC_DECODER_ROUTE_PROFILE_WINDOW_MS
#define AAC_DECODER_ROUTE_PROFILE_WINDOW_MS 10000U
#endif

#ifndef CONFIG_AAC_DECODER_ROUTE_PROFILE
#define CONFIG_AAC_DECODER_ROUTE_PROFILE 0
#endif

#ifndef CONFIG_AAC_ELD_DIAG
#define CONFIG_AAC_ELD_DIAG 0
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
#if CONFIG_AAC_ELD_DIAG
	uint32_t nav_window_start_us;
	uint32_t nav_decode_calls;
	uint32_t nav_decode_errors;
	uint32_t nav_over_budget;
	uint32_t nav_max_us;
	uint32_t nav_frame_budget_us;
	uint64_t nav_total_us;
	uint8_t nav_eld;
	uint8_t nav_task_logged;
	uint8_t qmf_lp_accepted;
#endif
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

static const char *aac_router_aot_name(uint32_t object_type)
{
	switch (object_type) {
	case 2U: return "AAC-LC";
	case 5U: return "HE-AAC";
	case 23U: return "AAC-LD";
	case 29U: return "HE-AACv2";
	case 39U: return "AAC-ELD";
	default: return "other";
	}
}

/* Called only after FDK accepts the ASC and Helix cannot use it. Keep the
 * diagnostic independent of the Helix eligibility check above. */
static void aac_router_describe_helix_reject(const aac_router_t *router,
	UCHAR *config[], const UINT length[], char *reason, size_t reason_size)
{
	uint32_t object_type;
	uint32_t bit_offset = 5U;
	uint32_t rate_index;
	uint32_t channel_config;

	if (router->transport != TT_MP4_RAW) {
		snprintf(reason, reason_size, "helix-requires-raw transport=%u",
			(unsigned int)router->transport);
		return;
	}
	if (router->layers != 1U) {
		snprintf(reason, reason_size, "helix-requires-one-layer layers=%lu",
			(unsigned long)router->layers);
		return;
	}
	if (config == NULL || length == NULL || config[0] == NULL) {
		snprintf(reason, reason_size, "missing-audio-specific-config");
		return;
	}
	if (length[0] < 2U) {
		snprintf(reason, reason_size, "short-audio-specific-config bytes=%u",
			(unsigned int)length[0]);
		return;
	}
	object_type = aac_router_read_bits(config[0], 0U, 5U);
	if (object_type == 31U) {
		object_type = 32U + aac_router_read_bits(config[0], bit_offset, 6U);
		bit_offset += 6U;
	}
	if (object_type != 2U) {
		snprintf(reason, reason_size,
			"helix-unsupported-profile profile=%s aot=%lu supported=AAC-LC asc_bytes=%u",
			aac_router_aot_name(object_type), (unsigned long)object_type,
			(unsigned int)length[0]);
		return;
	}
	if ((uint64_t)length[0] * 8U < bit_offset + 8U) {
		snprintf(reason, reason_size, "short-AAC-LC-config bytes=%u",
			(unsigned int)length[0]);
		return;
	}
	rate_index = aac_router_read_bits(config[0], bit_offset, 4U);
	bit_offset += 4U;
	if (rate_index >= 13U) {
		snprintf(reason, reason_size,
			"helix-unsupported-sample-rate-index index=%lu profile=AAC-LC",
			(unsigned long)rate_index);
		return;
	}
	channel_config = aac_router_read_bits(config[0], bit_offset, 4U);
	bit_offset += 4U;
	if (channel_config != 1U && channel_config != 2U) {
		snprintf(reason, reason_size,
			"helix-requires-mono-or-stereo channels=%lu profile=AAC-LC",
			(unsigned long)channel_config);
		return;
	}
	if ((uint64_t)length[0] * 8U < bit_offset + 1U) {
		snprintf(reason, reason_size, "short-AAC-LC-frame-config bytes=%u",
			(unsigned int)length[0]);
		return;
	}
	if (aac_router_read_bits(config[0], bit_offset, 1U) != 0U) {
		snprintf(reason, reason_size,
			"helix-requires-1024-sample-frames profile=AAC-LC");
		return;
	}
	snprintf(reason, reason_size, "helix-unsupported-config profile=AAC-LC");
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

#if CONFIG_AAC_ELD_DIAG && CARBOX_AAC_DECODER_MODE == CARBOX_AAC_DECODER_HELIX_AUTO
static void aac_router_nav_decode_report(aac_router_t *router,
	const char *event, uint32_t now_us)
{
	uint32_t window_ms;

	if (!router->nav_eld || router->nav_decode_calls == 0U) {
		return;
	}
	window_ms = (now_us - router->nav_window_start_us) / 1000U;
	printf("[AAC_ELD_DECODE] handle=%p event=%s window_ms=%lu "
		"calls=%lu errors=%lu avg_us=%llu max_us=%lu "
		"over_frame_budget=%lu frame_budget_us=%lu\n",
		(void *)router, event, (unsigned long)window_ms,
		(unsigned long)router->nav_decode_calls,
		(unsigned long)router->nav_decode_errors,
		(unsigned long long)(router->nav_total_us / router->nav_decode_calls),
		(unsigned long)router->nav_max_us,
		(unsigned long)router->nav_over_budget,
		(unsigned long)router->nav_frame_budget_us);
	router->nav_window_start_us = 0U;
	router->nav_decode_calls = 0U;
	router->nav_decode_errors = 0U;
	router->nav_over_budget = 0U;
	router->nav_max_us = 0U;
	router->nav_total_us = 0U;
}

static void aac_router_nav_decode_record(aac_router_t *router,
	uint32_t elapsed_us, AAC_DECODER_ERROR error)
{
	uint32_t now_us;

	if (!router->nav_eld) {
		return;
	}
	if (!router->nav_task_logged) {
		TaskHandle_t task = xTaskGetCurrentTaskHandle();
		printf("[AAC_ELD_TASK] handle=%p task=%s priority=%lu\n",
			(void *)router, pcTaskGetName(task),
			(unsigned long)uxTaskPriorityGet(task));
		router->nav_task_logged = 1U;
	}
	now_us = hal_read_curtime_us();
	if (router->nav_decode_calls == 0U) {
		router->nav_window_start_us = now_us;
	}
	router->nav_decode_calls++;
	router->nav_decode_errors += error != AAC_DEC_OK ? 1U : 0U;
	router->nav_over_budget += elapsed_us > router->nav_frame_budget_us ? 1U : 0U;
	router->nav_total_us += elapsed_us;
	if (elapsed_us > router->nav_max_us) {
		router->nav_max_us = elapsed_us;
	}
	if (now_us - router->nav_window_start_us >= 1000000U) {
		aac_router_nav_decode_report(router, "window", now_us);
	}
}

static void aac_router_nav_config(aac_router_t *router, UCHAR *config[],
	const UINT length[])
{
	CStreamInfo *info = aacDecoder_GetStreamInfo(router->fdk);
	uint32_t asc_prefix = 0U;
	uint32_t prefix_bytes = 0U;
	uint32_t i;

	/* aacNumChannels is populated by DecodeFrame, but channelConfig is
	 * available immediately after ConfigRaw. */
	if (info == NULL || info->aot != AOT_ER_AAC_ELD ||
	    info->aacSampleRate != 44100 || info->channelConfig != 1) {
		return;
	}
	router->nav_eld = 1U;
	if (info->aacSamplesPerFrame > 0) {
		router->nav_frame_budget_us =
			(uint32_t)(((uint64_t)info->aacSamplesPerFrame * 1000000U) /
				(uint32_t)info->aacSampleRate);
	}
	if (config != NULL && length != NULL && config[0] != NULL) {
		prefix_bytes = length[0] < 4U ? length[0] : 4U;
		for (i = 0U; i < prefix_bytes; ++i) {
			asc_prefix = (asc_prefix << 8) | config[0][i];
		}
	}
	printf("[AAC_ELD_CFG] handle=%p rate=%d channels=%d core_samples=%d "
		"frame_budget_us=%lu sbr=%u mps=%u qmf_lowpower_set=%u "
		"asc_bytes=%u asc_prefix_bytes=%lu asc_prefix=%08lx\n",
		(void *)router, info->aacSampleRate, info->channelConfig,
		info->aacSamplesPerFrame,
		(unsigned long)router->nav_frame_budget_us,
		(info->flags & AC_SBR_PRESENT) != 0U ? 1U : 0U,
		(info->flags & AC_MPS_PRESENT) != 0U ? 1U : 0U,
		(unsigned int)router->qmf_lp_accepted,
		length != NULL ? (unsigned int)length[0] : 0U,
		(unsigned long)prefix_bytes, (unsigned long)asc_prefix);
}
#endif

/* Configure the real FDK instance before the caller supplies ConfigRaw.
 * Bypass the router so this preference cannot switch a Helix decoder to FDK. */
static HANDLE_AACDECODER aac_router_open_fdk(TRANSPORT_TYPE transport,
	UINT layers, uint8_t *qmf_lp_accepted)
{
	HANDLE_AACDECODER fdk = __real_aacDecoder_Open(transport, layers);
	AAC_DECODER_ERROR error;

	if (qmf_lp_accepted != NULL) {
		*qmf_lp_accepted = 0U;
	}
	if (fdk == NULL) {
		return NULL;
	}
	error = __real_aacDecoder_SetParam(fdk, AAC_QMF_LOWPOWER, 1);
	if (qmf_lp_accepted != NULL && error == AAC_DEC_OK) {
		*qmf_lp_accepted = 1U;
	}
	if (error != AAC_DEC_OK) {
		/* An optional processing preference must not prevent audio startup. */
		printf("[AACROUTE] FDK QMF low-power request failed: 0x%x\n",
			(unsigned int)error);
	}
	return fdk;
}

HANDLE_AACDECODER __wrap_aacDecoder_Open(TRANSPORT_TYPE transport, UINT layers)
{
#if CARBOX_AAC_DECODER_MODE == CARBOX_AAC_DECODER_FDK_ONLY
	return aac_router_open_fdk(transport, layers, NULL);
#else
	aac_router_t *router = calloc(1U, sizeof(*router));

	if (router == NULL) {
		return NULL;
	}
#if CONFIG_AAC_ELD_DIAG
	router->fdk = aac_router_open_fdk(transport, layers,
		&router->qmf_lp_accepted);
#else
	router->fdk = aac_router_open_fdk(transport, layers, NULL);
#endif
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
	char reject_reason[128];

	if (router == NULL) {
		return __real_aacDecoder_ConfigRaw(decoder, config, length);
	}
	error = __real_aacDecoder_ConfigRaw(router->fdk, config, length);
	if (error != AAC_DEC_OK) {
		return error;
	}
#if CONFIG_AAC_ELD_DIAG
	aac_router_nav_decode_report(router, "reconfig", hal_read_curtime_us());
	router->nav_eld = 0U;
	router->nav_task_logged = 0U;
	aac_router_nav_config(router, config, length);
#endif
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
		aac_router_describe_helix_reject(router, config, length,
			reject_reason, sizeof(reject_reason));
		aac_router_select_fdk(router, reject_reason);
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
	{
		uint32_t elapsed_us = hal_read_curtime_us() - start_us;
		carbox_audio_decode_profile_record_backend(profile_name,
			elapsed_us, error != AAC_DEC_OK);
#if CONFIG_AAC_ELD_DIAG
		if (router != NULL && router->backend == AAC_ROUTER_BACKEND_FDK &&
		    flags == 0U) {
			aac_router_nav_decode_record(router, elapsed_us, error);
		}
#endif
	}
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
	AAC_DECODER_ERROR error;

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
	error = __real_aacDecoder_SetParam(router->fdk, parameter, value);
#if CONFIG_AAC_ELD_DIAG
	if (parameter == AAC_QMF_LOWPOWER) {
		router->qmf_lp_accepted = error == AAC_DEC_OK && value == 1 ? 1U : 0U;
		if (router->nav_eld) {
			printf("[AAC_ELD_QMF] handle=%p requested=%d result=0x%x "
				"lowpower_set=%u\n", (void *)router, value,
				(unsigned int)error,
				(unsigned int)router->qmf_lp_accepted);
		}
	}
#endif
	return error;
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
#if CONFIG_AAC_ELD_DIAG
	aac_router_nav_decode_report(router, "close", hal_read_curtime_us());
#endif
	router->magic = 0U;
	if (router->helix != NULL) {
		AACDeInitDecoder(router->helix);
	}
	__real_aacDecoder_Close(router->fdk);
	free(router);
#endif
}
