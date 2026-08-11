#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "aac_decoder_router.h"
#include "hal_timer.h"

#ifndef CONFIG_AUDIO_DECODE_PROFILE
#define CONFIG_AUDIO_DECODE_PROFILE 0
#endif

#ifndef AUDIO_DECODE_PROFILE_WINDOW_MS
#define AUDIO_DECODE_PROFILE_WINDOW_MS 10000U
#endif

#ifndef AUDIO_DECODE_PROFILE_SLOW_US
#define AUDIO_DECODE_PROFILE_SLOW_US 10000U
#endif

#if CONFIG_AUDIO_DECODE_PROFILE

typedef int32_t carbox_osstatus_t;
typedef int32_t carbox_aac_error_t;

typedef struct audio_decode_profile_stats_s {
	uint32_t window_start_us;
	uint32_t calls;
	uint32_t errors;
	uint32_t slow;
	uint64_t total_us;
	uint32_t max_us;
	uint32_t bins[7];
	uint64_t output_packets;
} audio_decode_profile_stats_t;

static audio_decode_profile_stats_t audio_converter_stats;
static audio_decode_profile_stats_t audio_fdk_stats;
static audio_decode_profile_stats_t audio_helix_stats;
static audio_decode_profile_stats_t audio_fallback_stats;

static uint32_t audio_decode_profile_bin(uint32_t elapsed_us)
{
	if (elapsed_us <= 500U) {
		return 0U;
	}
	if (elapsed_us <= 1000U) {
		return 1U;
	}
	if (elapsed_us <= 2000U) {
		return 2U;
	}
	if (elapsed_us <= 5000U) {
		return 3U;
	}
	if (elapsed_us <= 10000U) {
		return 4U;
	}
	if (elapsed_us <= 20000U) {
		return 5U;
	}
	return 6U;
}

static void audio_decode_profile_record(audio_decode_profile_stats_t *stats,
		const char *name, uint32_t elapsed_us, int failed,
		uint32_t output_packets)
{
	uint32_t now_us = hal_read_curtime_us();
	uint32_t window_us;
	uint32_t calls;
	uint32_t avg_us;
	uint32_t busy_x100;

	if (stats->window_start_us == 0U) {
		stats->window_start_us = now_us;
	}
	stats->calls++;
	stats->errors += failed ? 1U : 0U;
	stats->slow += elapsed_us >= AUDIO_DECODE_PROFILE_SLOW_US ? 1U : 0U;
	stats->total_us += elapsed_us;
	if (elapsed_us > stats->max_us) {
		stats->max_us = elapsed_us;
	}
	stats->bins[audio_decode_profile_bin(elapsed_us)]++;
	stats->output_packets += output_packets;

	window_us = now_us - stats->window_start_us;
	if (window_us < (AUDIO_DECODE_PROFILE_WINDOW_MS * 1000U)) {
		return;
	}

	calls = stats->calls != 0U ? stats->calls : 1U;
	avg_us = (uint32_t)(stats->total_us / calls);
	busy_x100 = window_us != 0U ?
		(uint32_t)((stats->total_us * 10000ULL) / window_us) : 0U;

	printf("[AUDDEC][%s] window_ms=%lu calls/errors/slow=%lu/%lu/%lu "
		"time_us total/avg/max=%llu/%lu/%lu busy=%lu.%02lu%% "
		"out_packets=%llu bins <=0.5/1/2/5/10/20/>20ms="
		"%lu/%lu/%lu/%lu/%lu/%lu/%lu\n",
		name,
		(unsigned long)(window_us / 1000U),
		(unsigned long)stats->calls,
		(unsigned long)stats->errors,
		(unsigned long)stats->slow,
		(unsigned long long)stats->total_us,
		(unsigned long)avg_us,
		(unsigned long)stats->max_us,
		(unsigned long)(busy_x100 / 100U),
		(unsigned long)(busy_x100 % 100U),
		(unsigned long long)stats->output_packets,
		(unsigned long)stats->bins[0],
		(unsigned long)stats->bins[1],
		(unsigned long)stats->bins[2],
		(unsigned long)stats->bins[3],
		(unsigned long)stats->bins[4],
		(unsigned long)stats->bins[5],
		(unsigned long)stats->bins[6]);

	memset(stats, 0, sizeof(*stats));
	stats->window_start_us = now_us;
}

extern carbox_osstatus_t __real_AudioConverterFillComplexBuffer(
		void *converter, void *input_proc, void *input_context,
		uint32_t *output_packet_count, void *output_data,
		void *output_packet_descriptions);

carbox_osstatus_t __wrap_AudioConverterFillComplexBuffer(
		void *converter, void *input_proc, void *input_context,
		uint32_t *output_packet_count, void *output_data,
		void *output_packet_descriptions)
{
	uint32_t start_us = hal_read_curtime_us();
	carbox_osstatus_t result = __real_AudioConverterFillComplexBuffer(
		converter, input_proc, input_context, output_packet_count,
		output_data, output_packet_descriptions);
	uint32_t elapsed_us = hal_read_curtime_us() - start_us;
	uint32_t packets = output_packet_count != NULL ? *output_packet_count : 0U;

	audio_decode_profile_record(&audio_converter_stats, "CONVERTER",
		elapsed_us, result != 0, packets);
	return result;
}

void carbox_audio_decode_profile_record_backend(const char *backend,
		uint32_t elapsed_us, int failed)
{
	audio_decode_profile_stats_t *stats = &audio_fdk_stats;

	if (strcmp(backend, "HELIX") == 0) {
		stats = &audio_helix_stats;
	} else if (strcmp(backend, "FDK-FALLBACK") == 0) {
		stats = &audio_fallback_stats;
	}
	audio_decode_profile_record(stats, backend, elapsed_us, failed, 0U);
}

#else

void carbox_audio_decode_profile_record_backend(const char *backend,
		uint32_t elapsed_us, int failed)
{
	(void)backend;
	(void)elapsed_us;
	(void)failed;
}

#endif /* CONFIG_AUDIO_DECODE_PROFILE */
