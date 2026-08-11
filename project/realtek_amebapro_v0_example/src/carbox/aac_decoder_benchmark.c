#include "aac_decoder_benchmark.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "aacdec.h"

/* Ameba basic_types.h exposes these names as macros, while FDK declares
 * matching fixed-width typedefs in machine_type.h. */
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
#include "task.h"

#ifndef CONFIG_AAC_DECODER_BENCHMARK
#define CONFIG_AAC_DECODER_BENCHMARK 0
#endif

#ifndef AAC_DECODER_BENCHMARK_FILE
#define AAC_DECODER_BENCHMARK_FILE "fat:/bear-audio-lc-aac.aac"
#endif

#ifndef AAC_DECODER_BENCHMARK_LOOPS
#define AAC_DECODER_BENCHMARK_LOOPS 1U
#endif

#ifndef AAC_DECODER_BENCHMARK_START_DELAY_MS
#define AAC_DECODER_BENCHMARK_START_DELAY_MS 3000U
#endif

#ifndef AAC_DECODER_BENCHMARK_REPORT_MS
#define AAC_DECODER_BENCHMARK_REPORT_MS 10000U
#endif

#ifndef AAC_DECODER_BENCHMARK_TASK_PRIORITY
#define AAC_DECODER_BENCHMARK_TASK_PRIORITY 1U
#endif

#ifndef AAC_DECODER_BENCHMARK_RUN_PRIORITY
#define AAC_DECODER_BENCHMARK_RUN_PRIORITY (configMAX_PRIORITIES - 1U)
#endif

#ifndef AAC_DECODER_BENCHMARK_TASK_STACK
#define AAC_DECODER_BENCHMARK_TASK_STACK 2048U
#endif

#define AAC_BENCHMARK_PCM_SAMPLES 4096U

#if CONFIG_AAC_DECODER_BENCHMARK

/* The production decoder APIs are routed through the selectable backend.
 * Benchmark FDK measurements must bypass that router completely. */
extern AAC_DECODER_ERROR __real_aacDecoder_DecodeFrame(
	HANDLE_AACDECODER decoder, INT_PCM *output, INT output_samples,
	UINT flags);
extern HANDLE_AACDECODER __real_aacDecoder_Open(TRANSPORT_TYPE transport,
	UINT layers);
extern AAC_DECODER_ERROR __real_aacDecoder_Fill(HANDLE_AACDECODER decoder,
	UCHAR *buffer[], const UINT size[], UINT *valid);
extern void __real_aacDecoder_Close(HANDLE_AACDECODER decoder);
#define aac_benchmark_fdk_decode __real_aacDecoder_DecodeFrame
#define aac_benchmark_fdk_open __real_aacDecoder_Open
#define aac_benchmark_fdk_fill __real_aacDecoder_Fill
#define aac_benchmark_fdk_close __real_aacDecoder_Close

typedef struct aac_benchmark_result_s {
	uint32_t frames;
	uint32_t errors;
	uint32_t samples;
	uint32_t sample_rate;
	uint32_t channels;
	uint64_t total_us;
	uint32_t max_us;
} aac_benchmark_result_t;

typedef struct aac_adts_frame_s {
	const uint8_t *data;
	uint32_t size;
} aac_adts_frame_t;

static uint32_t aac_benchmark_ticks(uint32_t milliseconds)
{
	return (milliseconds + portTICK_PERIOD_MS - 1U) / portTICK_PERIOD_MS;
}

static int aac_benchmark_next_adts(const uint8_t *data, uint32_t size,
		uint32_t *offset, aac_adts_frame_t *frame)
{
	uint32_t pos = *offset;
	uint32_t frame_size;

	while (pos + 7U <= size &&
	       !(data[pos] == 0xffU && (data[pos + 1U] & 0xf6U) == 0xf0U)) {
		pos++;
	}
	if (pos + 7U > size) {
		return 0;
	}
	frame_size = ((uint32_t)(data[pos + 3U] & 0x03U) << 11) |
		((uint32_t)data[pos + 4U] << 3) |
		((uint32_t)data[pos + 5U] >> 5);
	if (frame_size < 7U || frame_size > size - pos) {
		return -1;
	}
	frame->data = data + pos;
	frame->size = frame_size;
	*offset = pos + frame_size;
	return 1;
}

static uint8_t *aac_benchmark_load_file(uint32_t *file_size)
{
	FILE *file;
	long length;
	uint8_t *buffer;

	file = fopen(AAC_DECODER_BENCHMARK_FILE, "rb");
	if (file == NULL) {
		printf("[AACBENCH] open failed file=%s\n", AAC_DECODER_BENCHMARK_FILE);
		return NULL;
	}
	if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) <= 0 ||
	    fseek(file, 0, SEEK_SET) != 0) {
		printf("[AACBENCH] size failed file=%s\n", AAC_DECODER_BENCHMARK_FILE);
		fclose(file);
		return NULL;
	}
	buffer = malloc((size_t)length);
	if (buffer == NULL) {
		printf("[AACBENCH] malloc failed bytes=%ld\n", length);
		fclose(file);
		return NULL;
	}
	if (fread(buffer, 1U, (size_t)length, file) != (size_t)length) {
		printf("[AACBENCH] read failed bytes=%ld\n", length);
		free(buffer);
		fclose(file);
		return NULL;
	}
	fclose(file);
	*file_size = (uint32_t)length;
	return buffer;
}

static uint32_t aac_benchmark_count_frames(const uint8_t *data, uint32_t size)
{
	aac_adts_frame_t frame;
	uint32_t offset = 0U;
	uint32_t count = 0U;
	int status;

	while ((status = aac_benchmark_next_adts(data, size, &offset, &frame)) > 0) {
		count++;
	}
	return status == 0 && offset == size ? count : 0U;
}

static void aac_benchmark_failure_loop(const char *stage)
{
	for (;;) {
		printf("[AACBENCH] state=failed stage=%s file=%s\n",
			stage, AAC_DECODER_BENCHMARK_FILE);
		vTaskDelay((TickType_t)aac_benchmark_ticks(
			AAC_DECODER_BENCHMARK_REPORT_MS));
	}
}

static int aac_benchmark_fdk(const uint8_t *data, uint32_t size,
		int16_t *pcm, aac_benchmark_result_t *result)
{
	HANDLE_AACDECODER decoder;
	uint32_t loop;

	decoder = aac_benchmark_fdk_open(TT_MP4_ADTS, 1U);
	if (decoder == NULL) {
		return -1;
	}
	for (loop = 0U; loop < AAC_DECODER_BENCHMARK_LOOPS; loop++) {
		aac_adts_frame_t frame;
		uint32_t offset = 0U;
		int status;

		while ((status = aac_benchmark_next_adts(data, size, &offset, &frame)) > 0) {
			UCHAR *input = (UCHAR *)frame.data;
			UINT input_size = frame.size;
			UINT valid = frame.size;
			AAC_DECODER_ERROR error;
			uint32_t start_us;
			uint32_t elapsed_us;

			error = aac_benchmark_fdk_fill(decoder, &input, &input_size,
				&valid);
			if (error != AAC_DEC_OK || valid != 0U) {
				result->errors++;
				continue;
			}
			start_us = hal_read_curtime_us();
			error = aac_benchmark_fdk_decode(decoder, pcm,
				AAC_BENCHMARK_PCM_SAMPLES, 0U);
			elapsed_us = hal_read_curtime_us() - start_us;
			result->frames++;
			result->total_us += elapsed_us;
			if (elapsed_us > result->max_us) {
				result->max_us = elapsed_us;
			}
			if (error != AAC_DEC_OK) {
				result->errors++;
			} else {
				CStreamInfo *info = aacDecoder_GetStreamInfo(decoder);
				if (info != NULL) {
					result->sample_rate = (uint32_t)info->sampleRate;
					result->channels = (uint32_t)info->numChannels;
					result->samples += (uint32_t)info->frameSize *
						(uint32_t)info->numChannels;
				}
			}
		}
		if (status < 0) {
			result->errors++;
		}
	}
	aac_benchmark_fdk_close(decoder);
	return 0;
}

static int aac_benchmark_helix(const uint8_t *data, uint32_t size,
		int16_t *pcm, aac_benchmark_result_t *result)
{
	HAACDecoder decoder;
	uint32_t loop;

	decoder = AACInitDecoder();
	if (decoder == NULL) {
		return -1;
	}
	for (loop = 0U; loop < AAC_DECODER_BENCHMARK_LOOPS; loop++) {
		aac_adts_frame_t frame;
		uint32_t offset = 0U;
		int status;

		while ((status = aac_benchmark_next_adts(data, size, &offset, &frame)) > 0) {
			unsigned char *input = (unsigned char *)frame.data;
			int bytes_left = (int)frame.size;
			AACFrameInfo info;
			uint32_t start_us = hal_read_curtime_us();
			int error = AACDecode(decoder, &input, &bytes_left, pcm);
			uint32_t elapsed_us = hal_read_curtime_us() - start_us;

			result->frames++;
			result->total_us += elapsed_us;
			if (elapsed_us > result->max_us) {
				result->max_us = elapsed_us;
			}
			if (error != 0) {
				result->errors++;
			} else {
				memset(&info, 0, sizeof(info));
				AACGetLastFrameInfo(decoder, &info);
				result->sample_rate = (uint32_t)info.sampRateOut;
				result->channels = (uint32_t)info.nChans;
				result->samples += (uint32_t)info.outputSamps;
			}
			if (bytes_left != 0 && error == 0) {
				result->errors++;
			}
		}
		if (status < 0) {
			result->errors++;
		}
	}
	AACDeInitDecoder(decoder);
	return 0;
}

static void aac_benchmark_report(uint32_t run, const char *order,
		uint32_t file_size, uint32_t source_frames,
		const aac_benchmark_result_t *fdk,
		const aac_benchmark_result_t *helix)
{
	uint64_t fdk_audio_us = fdk->sample_rate != 0U && fdk->channels != 0U ?
		((uint64_t)fdk->samples * 1000000ULL) /
		((uint64_t)fdk->sample_rate * fdk->channels) : 0U;
	uint64_t helix_audio_us = helix->sample_rate != 0U && helix->channels != 0U ?
		((uint64_t)helix->samples * 1000000ULL) /
		((uint64_t)helix->sample_rate * helix->channels) : 0U;
	uint32_t fdk_avg = fdk->frames != 0U ?
		(uint32_t)(fdk->total_us / fdk->frames) : 0U;
	uint32_t helix_avg = helix->frames != 0U ?
		(uint32_t)(helix->total_us / helix->frames) : 0U;
	uint32_t fdk_rt_x100 = fdk_audio_us != 0U ?
		(uint32_t)((fdk->total_us * 10000ULL) / fdk_audio_us) : 0U;
	uint32_t helix_rt_x100 = helix_audio_us != 0U ?
		(uint32_t)((helix->total_us * 10000ULL) / helix_audio_us) : 0U;
	uint32_t ratio_x100 = helix->total_us != 0U ?
		(uint32_t)((fdk->total_us * 100ULL) / helix->total_us) : 0U;
	uint32_t format_match = fdk->sample_rate == helix->sample_rate &&
		fdk->channels == helix->channels;
	uint32_t sample_match = fdk->samples == helix->samples;

	printf("[AACBENCH] run=%lu priority=%lu order=%s file=%s bytes=%lu loops=%lu source_frames=%lu "
		"FDK frames/errors/us_total/avg/max=%lu/%lu/%llu/%lu/%lu rt=%lu.%02lu%% fmt=%lu/%lu "
		"HELIX frames/errors/us_total/avg/max=%lu/%lu/%llu/%lu/%lu rt=%lu.%02lu%% fmt=%lu/%lu "
		"match_fmt/samples=%lu/%lu time_ratio_fdk/helix=%lu.%02lux\n",
		(unsigned long)run,
		(unsigned long)AAC_DECODER_BENCHMARK_RUN_PRIORITY,
		order,
		AAC_DECODER_BENCHMARK_FILE,
		(unsigned long)file_size,
		(unsigned long)AAC_DECODER_BENCHMARK_LOOPS,
		(unsigned long)source_frames,
		(unsigned long)fdk->frames,
		(unsigned long)fdk->errors,
		(unsigned long long)fdk->total_us,
		(unsigned long)fdk_avg,
		(unsigned long)fdk->max_us,
		(unsigned long)(fdk_rt_x100 / 100U),
		(unsigned long)(fdk_rt_x100 % 100U),
		(unsigned long)fdk->sample_rate,
		(unsigned long)fdk->channels,
		(unsigned long)helix->frames,
		(unsigned long)helix->errors,
		(unsigned long long)helix->total_us,
		(unsigned long)helix_avg,
		(unsigned long)helix->max_us,
		(unsigned long)(helix_rt_x100 / 100U),
		(unsigned long)(helix_rt_x100 % 100U),
		(unsigned long)helix->sample_rate,
		(unsigned long)helix->channels,
		(unsigned long)format_match,
		(unsigned long)sample_match,
		(unsigned long)(ratio_x100 / 100U),
		(unsigned long)(ratio_x100 % 100U));
}

static void aac_benchmark_task(void *parameter)
{
	uint8_t *data;
	int16_t *pcm;
	uint32_t file_size = 0U;
	uint32_t source_frames;
	uint32_t run = 0U;
	TickType_t last_wake;
	aac_benchmark_result_t fdk;
	aac_benchmark_result_t helix;

	(void)parameter;
	vTaskDelay((TickType_t)aac_benchmark_ticks(
		AAC_DECODER_BENCHMARK_START_DELAY_MS));
	data = aac_benchmark_load_file(&file_size);
	if (data == NULL) {
		aac_benchmark_failure_loop("load");
		return;
	}
	source_frames = aac_benchmark_count_frames(data, file_size);
	if (source_frames == 0U) {
		printf("[AACBENCH] invalid ADTS file=%s bytes=%lu\n",
			AAC_DECODER_BENCHMARK_FILE, (unsigned long)file_size);
		free(data);
		aac_benchmark_failure_loop("adts");
		return;
	}
	pcm = malloc(AAC_BENCHMARK_PCM_SAMPLES * sizeof(*pcm));
	if (pcm == NULL) {
		printf("[AACBENCH] PCM malloc failed samples=%lu\n",
			(unsigned long)AAC_BENCHMARK_PCM_SAMPLES);
		free(data);
		aac_benchmark_failure_loop("pcm_alloc");
		return;
	}
	printf("[AACBENCH] loaded file=%s bytes=%lu frames=%lu resident=dram "
		"run_priority=%lu period_ms=%lu\n",
		AAC_DECODER_BENCHMARK_FILE, (unsigned long)file_size,
		(unsigned long)source_frames,
		(unsigned long)AAC_DECODER_BENCHMARK_RUN_PRIORITY,
		(unsigned long)AAC_DECODER_BENCHMARK_REPORT_MS);
	last_wake = xTaskGetTickCount();
	for (;;) {
		const char *order;

		run++;
		memset(&fdk, 0, sizeof(fdk));
		memset(&helix, 0, sizeof(helix));
		vTaskPrioritySet(NULL, AAC_DECODER_BENCHMARK_RUN_PRIORITY);
		if ((run & 1U) != 0U) {
			order = "FDK-HELIX";
			if (aac_benchmark_fdk(data, file_size, pcm, &fdk) != 0) {
				fdk.errors++;
			}
			if (aac_benchmark_helix(data, file_size, pcm, &helix) != 0) {
				helix.errors++;
			}
		} else {
			order = "HELIX-FDK";
			if (aac_benchmark_helix(data, file_size, pcm, &helix) != 0) {
				helix.errors++;
			}
			if (aac_benchmark_fdk(data, file_size, pcm, &fdk) != 0) {
				fdk.errors++;
			}
		}
		vTaskPrioritySet(NULL, AAC_DECODER_BENCHMARK_TASK_PRIORITY);
		aac_benchmark_report(run, order, file_size, source_frames,
			&fdk, &helix);
		vTaskDelayUntil(&last_wake, (TickType_t)aac_benchmark_ticks(
			AAC_DECODER_BENCHMARK_REPORT_MS));
	}
}

void carbox_aac_decoder_benchmark_start(void)
{
	BaseType_t status;

	printf("[AACBENCH] enabled file=%s loops=%lu start_delay_ms=%lu report_ms=%lu "
		"wait/run_priority=%lu/%lu code=dram\n",
		AAC_DECODER_BENCHMARK_FILE,
		(unsigned long)AAC_DECODER_BENCHMARK_LOOPS,
		(unsigned long)AAC_DECODER_BENCHMARK_START_DELAY_MS,
		(unsigned long)AAC_DECODER_BENCHMARK_REPORT_MS,
		(unsigned long)AAC_DECODER_BENCHMARK_TASK_PRIORITY,
		(unsigned long)AAC_DECODER_BENCHMARK_RUN_PRIORITY);
	status = xTaskCreate(aac_benchmark_task, "aacbench",
		AAC_DECODER_BENCHMARK_TASK_STACK, NULL,
		AAC_DECODER_BENCHMARK_TASK_PRIORITY, NULL);

	if (status != pdPASS) {
		printf("[AACBENCH] task create failed\n");
	}
}

#else

void carbox_aac_decoder_benchmark_start(void)
{
}

#endif /* CONFIG_AAC_DECODER_BENCHMARK */
