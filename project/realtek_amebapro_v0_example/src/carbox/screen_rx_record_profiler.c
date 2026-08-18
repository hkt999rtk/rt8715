#include "screen_rx_record_profiler.h"

#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "diag.h"

#ifndef CONFIG_SCREEN_RX_RECORD_PROFILE
#define CONFIG_SCREEN_RX_RECORD_PROFILE 0
#endif

#if CONFIG_SCREEN_RX_RECORD_PROFILE

#define SCREEN_RX_RECORD_SLOTS 16U
#define SCREEN_RX_RECORD_MAX_BODY (4U * 1024U * 1024U)

typedef struct screen_rx_record_slot_s {
	void *pointer;
	uint32_t allocation_length;
	uint32_t declared_length;
	uint8_t frame_type;
	uint8_t sent_video;
} screen_rx_record_slot_t;

typedef struct screen_rx_record_stats_s {
	uint32_t headers;
	uint32_t header_valid;
	uint32_t header_invalid;
	uint32_t header_overwrite;
	uint32_t type[6];
	uint32_t type_other;
	uint64_t declared_bytes;
	uint32_t declared_max;
	uint32_t payload_calls;
	uint32_t payload_full;
	uint32_t payload_partial;
	uint32_t payload_error;
	uint64_t payload_requested_bytes;
	uint64_t payload_received_bytes;

	uint32_t allocations;
	uint32_t allocation_failures;
	uint32_t allocation_exact;
	uint32_t allocation_mismatch;
	uint32_t allocation_without_header;
	uint32_t allocation_table_full;
	uint64_t allocation_bytes;
	uint32_t frees;
	uint32_t free_delivered;
	uint32_t free_dropped;
	uint32_t free_untracked;
	uint64_t delivered_bytes;
	uint64_t dropped_bytes;
	uint32_t send_video;
	uint32_t send_video_matched;
	uint32_t send_video_unmatched;
	uint64_t send_video_bytes;
	uint32_t live_max;

	uint32_t crypto_init;
	uint32_t crypto_aad_calls;
	uint64_t crypto_aad_bytes;
	uint32_t crypto_decrypt_calls;
	uint64_t crypto_decrypt_input;
	uint64_t crypto_decrypt_output;
	uint32_t crypto_decrypt_short;
	uint32_t crypto_verify_calls;
	uint32_t crypto_verify_ok;
	uint32_t crypto_verify_error;
	uint32_t crypto_verify_no_status;
	uint64_t crypto_verify_output;
	int32_t crypto_last_error;
	uint32_t crypto_final_calls;
	uint64_t crypto_final_output;
} screen_rx_record_stats_t;

static TaskHandle_t screen_rx_receiver_task;
static screen_rx_record_stats_t screen_rx_stats;
static screen_rx_record_slot_t screen_rx_slots[SCREEN_RX_RECORD_SLOTS];
static uint32_t screen_rx_pending_length;
static uint8_t screen_rx_pending_type;
static uint8_t screen_rx_pending_valid;

static uint32_t screen_rx_load_le32(const uint8_t *data)
{
	return (uint32_t)data[0] |
	       ((uint32_t)data[1] << 8) |
	       ((uint32_t)data[2] << 16) |
	       ((uint32_t)data[3] << 24);
}

static int screen_rx_is_receiver(void)
{
	TaskHandle_t current = xTaskGetCurrentTaskHandle();
	const char *name;

	if (current == screen_rx_receiver_task) {
		return current != NULL;
	}
	name = pcTaskGetName(current);
	if ((name != NULL) &&
	    (strcmp(name, "AirPlayScreenReceiver") == 0)) {
		screen_rx_receiver_task = current;
		return 1;
	}
	return 0;
}

static uint32_t screen_rx_live_locked(void)
{
	uint32_t live = 0U;
	uint32_t i;

	for (i = 0U; i < SCREEN_RX_RECORD_SLOTS; ++i) {
		live += screen_rx_slots[i].pointer != NULL ? 1U : 0U;
	}
	return live;
}

void carbox_screen_rx_record_recv(const void *buffer, size_t requested,
				  int result)
{
	if (!screen_rx_is_receiver()) {
		return;
	}

	if (requested == 128U) {
		uint32_t body_length = 0U;
		uint8_t frame_type = 0xffU;
		int valid = 0;

		if ((result == 128) && (buffer != NULL)) {
			const uint8_t *header = (const uint8_t *)buffer;

			body_length = screen_rx_load_le32(header);
			frame_type = header[4];
			valid = (body_length > 0U) &&
				(body_length <= SCREEN_RX_RECORD_MAX_BODY);
		}
		taskENTER_CRITICAL();
		screen_rx_stats.headers++;
		if (valid) {
			screen_rx_stats.header_valid++;
			if (screen_rx_pending_valid) {
				screen_rx_stats.header_overwrite++;
			}
			screen_rx_pending_length = body_length;
			screen_rx_pending_type = frame_type;
			screen_rx_pending_valid = 1U;
			screen_rx_stats.declared_bytes += body_length;
			if (body_length > screen_rx_stats.declared_max) {
				screen_rx_stats.declared_max = body_length;
			}
			if (frame_type < 6U) {
				screen_rx_stats.type[frame_type]++;
			} else {
				screen_rx_stats.type_other++;
			}
		} else {
			screen_rx_stats.header_invalid++;
		}
		taskEXIT_CRITICAL();
		return;
	}

	if (requested > 128U) {
		taskENTER_CRITICAL();
		screen_rx_stats.payload_calls++;
		screen_rx_stats.payload_requested_bytes += requested;
		if (result > 0) {
			screen_rx_stats.payload_received_bytes += (uint32_t)result;
			if ((size_t)result == requested) {
				screen_rx_stats.payload_full++;
			} else {
				screen_rx_stats.payload_partial++;
			}
		} else {
			screen_rx_stats.payload_error++;
		}
		taskEXIT_CRITICAL();
	}
}

void carbox_screen_rx_record_alloc(void *pointer, size_t length)
{
	uint32_t i;
	uint32_t live;

	if (!screen_rx_is_receiver() || (length <= 128U)) {
		return;
	}
	taskENTER_CRITICAL();
	screen_rx_stats.allocations++;
	if (pointer == NULL) {
		screen_rx_stats.allocation_failures++;
		taskEXIT_CRITICAL();
		return;
	}
	screen_rx_stats.allocation_bytes += length;
	if (screen_rx_pending_valid) {
		if (length == screen_rx_pending_length) {
			screen_rx_stats.allocation_exact++;
		} else {
			screen_rx_stats.allocation_mismatch++;
		}
	} else {
		screen_rx_stats.allocation_without_header++;
	}
	for (i = 0U; i < SCREEN_RX_RECORD_SLOTS; ++i) {
		if (screen_rx_slots[i].pointer == NULL) {
			screen_rx_slots[i].pointer = pointer;
			screen_rx_slots[i].allocation_length = (uint32_t)length;
			screen_rx_slots[i].declared_length =
				screen_rx_pending_valid ? screen_rx_pending_length : 0U;
			screen_rx_slots[i].frame_type =
				screen_rx_pending_valid ? screen_rx_pending_type : 0xffU;
			screen_rx_slots[i].sent_video = 0U;
			break;
		}
	}
	if (i == SCREEN_RX_RECORD_SLOTS) {
		screen_rx_stats.allocation_table_full++;
	}
	screen_rx_pending_valid = 0U;
	live = screen_rx_live_locked();
	if (live > screen_rx_stats.live_max) {
		screen_rx_stats.live_max = live;
	}
	taskEXIT_CRITICAL();
}

void carbox_screen_rx_record_send_video(const void *pointer, int length)
{
	uint32_t i;

	if (!screen_rx_is_receiver()) {
		return;
	}
	taskENTER_CRITICAL();
	screen_rx_stats.send_video++;
	if (length > 0) {
		screen_rx_stats.send_video_bytes += (uint32_t)length;
	}
	for (i = 0U; i < SCREEN_RX_RECORD_SLOTS; ++i) {
		if (screen_rx_slots[i].pointer == pointer) {
			screen_rx_slots[i].sent_video = 1U;
			screen_rx_stats.send_video_matched++;
			break;
		}
	}
	if (i == SCREEN_RX_RECORD_SLOTS) {
		screen_rx_stats.send_video_unmatched++;
	}
	taskEXIT_CRITICAL();
}

void carbox_screen_rx_record_free(void *pointer)
{
	uint32_t i;

	if (!screen_rx_is_receiver() || (pointer == NULL)) {
		return;
	}
	taskENTER_CRITICAL();
	screen_rx_stats.frees++;
	for (i = 0U; i < SCREEN_RX_RECORD_SLOTS; ++i) {
		if (screen_rx_slots[i].pointer == pointer) {
			if (screen_rx_slots[i].sent_video) {
				screen_rx_stats.free_delivered++;
				screen_rx_stats.delivered_bytes +=
					screen_rx_slots[i].allocation_length;
			} else {
				screen_rx_stats.free_dropped++;
				screen_rx_stats.dropped_bytes +=
					screen_rx_slots[i].allocation_length;
			}
			screen_rx_slots[i] = (screen_rx_record_slot_t){ 0 };
			break;
		}
	}
	if (i == SCREEN_RX_RECORD_SLOTS) {
		screen_rx_stats.free_untracked++;
	}
	taskEXIT_CRITICAL();
}

void carbox_screen_rx_crypto_init(void)
{
	if (!screen_rx_is_receiver()) return;
	taskENTER_CRITICAL();
	screen_rx_stats.crypto_init++;
	taskEXIT_CRITICAL();
}

void carbox_screen_rx_crypto_aad(size_t length)
{
	if (!screen_rx_is_receiver()) return;
	taskENTER_CRITICAL();
	screen_rx_stats.crypto_aad_calls++;
	screen_rx_stats.crypto_aad_bytes += length;
	taskEXIT_CRITICAL();
}

void carbox_screen_rx_crypto_decrypt(size_t input, size_t output)
{
	if (!screen_rx_is_receiver()) return;
	taskENTER_CRITICAL();
	screen_rx_stats.crypto_decrypt_calls++;
	screen_rx_stats.crypto_decrypt_input += input;
	screen_rx_stats.crypto_decrypt_output += output;
	if (output != input) screen_rx_stats.crypto_decrypt_short++;
	taskEXIT_CRITICAL();
}

void carbox_screen_rx_crypto_verify(size_t output, const int32_t *error)
{
	if (!screen_rx_is_receiver()) return;
	taskENTER_CRITICAL();
	screen_rx_stats.crypto_verify_calls++;
	screen_rx_stats.crypto_verify_output += output;
	if (error == NULL) {
		screen_rx_stats.crypto_verify_no_status++;
	} else if (*error == 0) {
		screen_rx_stats.crypto_verify_ok++;
	} else {
		screen_rx_stats.crypto_verify_error++;
		screen_rx_stats.crypto_last_error = *error;
	}
	taskEXIT_CRITICAL();
}

void carbox_screen_rx_crypto_final(size_t output)
{
	if (!screen_rx_is_receiver()) return;
	taskENTER_CRITICAL();
	screen_rx_stats.crypto_final_calls++;
	screen_rx_stats.crypto_final_output += output;
	taskEXIT_CRITICAL();
}

void carbox_screen_rx_record_profiler_report(uint32_t sequence)
{
	screen_rx_record_stats_t stats;
	uint32_t live;
	uint32_t pending_length;
	uint32_t pending;

	taskENTER_CRITICAL();
	stats = screen_rx_stats;
	screen_rx_stats = (screen_rx_record_stats_t){ 0 };
	live = screen_rx_live_locked();
	pending = screen_rx_pending_valid;
	pending_length = screen_rx_pending_length;
	taskEXIT_CRITICAL();

	rt_printf("[SCREENRXREC][%lu] header all/valid/invalid/overwrite="
		  "%lu/%lu/%lu/%lu type0-5/other="
		  "%lu/%lu/%lu/%lu/%lu/%lu/%lu declared_B total/avg/max="
		  "%llu/%llu/%lu pending/len=%lu/%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.headers,
		  (unsigned long)stats.header_valid,
		  (unsigned long)stats.header_invalid,
		  (unsigned long)stats.header_overwrite,
		  (unsigned long)stats.type[0], (unsigned long)stats.type[1],
		  (unsigned long)stats.type[2], (unsigned long)stats.type[3],
		  (unsigned long)stats.type[4], (unsigned long)stats.type[5],
		  (unsigned long)stats.type_other,
		  (unsigned long long)stats.declared_bytes,
		  (unsigned long long)(stats.header_valid != 0U ?
			stats.declared_bytes / stats.header_valid : 0U),
		  (unsigned long)stats.declared_max,
		  (unsigned long)pending, (unsigned long)pending_length);
	rt_printf("[SCREENRXREC][%lu] payload calls/full/partial/error="
		  "%lu/%lu/%lu/%lu bytes requested/received=%llu/%llu "
		  "alloc all/fail/exact/mismatch/no_header/table_full="
		  "%lu/%lu/%lu/%lu/%lu/%lu bytes=%llu live/max=%lu/%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.payload_calls,
		  (unsigned long)stats.payload_full,
		  (unsigned long)stats.payload_partial,
		  (unsigned long)stats.payload_error,
		  (unsigned long long)stats.payload_requested_bytes,
		  (unsigned long long)stats.payload_received_bytes,
		  (unsigned long)stats.allocations,
		  (unsigned long)stats.allocation_failures,
		  (unsigned long)stats.allocation_exact,
		  (unsigned long)stats.allocation_mismatch,
		  (unsigned long)stats.allocation_without_header,
		  (unsigned long)stats.allocation_table_full,
		  (unsigned long long)stats.allocation_bytes,
		  (unsigned long)live, (unsigned long)stats.live_max);
	rt_printf("[SCREENRXREC][%lu] free all/delivered/dropped/untracked="
		  "%lu/%lu/%lu/%lu bytes delivered/dropped=%llu/%llu "
		  "send_video all/matched/unmatched/bytes=%lu/%lu/%lu/%llu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.frees,
		  (unsigned long)stats.free_delivered,
		  (unsigned long)stats.free_dropped,
		  (unsigned long)stats.free_untracked,
		  (unsigned long long)stats.delivered_bytes,
		  (unsigned long long)stats.dropped_bytes,
		  (unsigned long)stats.send_video,
		  (unsigned long)stats.send_video_matched,
		  (unsigned long)stats.send_video_unmatched,
		  (unsigned long long)stats.send_video_bytes);
	rt_printf("[SCREENRXCRYPTO][%lu] init/aad/decrypt/short="
		  "%lu/%lu/%lu/%lu aad_B=%llu decrypt_B in/out=%llu/%llu "
		  "verify all/ok/error/no_status=%lu/%lu/%lu/%lu out_B=%llu "
		  "last_error=%ld final calls/out_B=%lu/%llu observation_only=1\r\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.crypto_init,
		  (unsigned long)stats.crypto_aad_calls,
		  (unsigned long)stats.crypto_decrypt_calls,
		  (unsigned long)stats.crypto_decrypt_short,
		  (unsigned long long)stats.crypto_aad_bytes,
		  (unsigned long long)stats.crypto_decrypt_input,
		  (unsigned long long)stats.crypto_decrypt_output,
		  (unsigned long)stats.crypto_verify_calls,
		  (unsigned long)stats.crypto_verify_ok,
		  (unsigned long)stats.crypto_verify_error,
		  (unsigned long)stats.crypto_verify_no_status,
		  (unsigned long long)stats.crypto_verify_output,
		  (long)stats.crypto_last_error,
		  (unsigned long)stats.crypto_final_calls,
		  (unsigned long long)stats.crypto_final_output);
}

#else

void carbox_screen_rx_record_recv(const void *buffer, size_t requested,
				  int result)
{ (void)buffer; (void)requested; (void)result; }
void carbox_screen_rx_record_alloc(void *pointer, size_t length)
{ (void)pointer; (void)length; }
void carbox_screen_rx_record_free(void *pointer) { (void)pointer; }
void carbox_screen_rx_record_send_video(const void *pointer, int length)
{ (void)pointer; (void)length; }
void carbox_screen_rx_crypto_init(void) { }
void carbox_screen_rx_crypto_aad(size_t length) { (void)length; }
void carbox_screen_rx_crypto_decrypt(size_t input, size_t output)
{ (void)input; (void)output; }
void carbox_screen_rx_crypto_verify(size_t output, const int32_t *error)
{ (void)output; (void)error; }
void carbox_screen_rx_crypto_final(size_t output) { (void)output; }
void carbox_screen_rx_record_profiler_report(uint32_t sequence)
{ (void)sequence; }

#endif
