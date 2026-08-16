#include "ncm_wrap_profiler.h"

#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "cmsis.h"
#include "diag.h"

#ifndef CONFIG_NCM_WRAP_PROFILE
#define CONFIG_NCM_WRAP_PROFILE 0
#endif

#ifndef CONFIG_NCM_WRAP_COPY_ELIDE
#define CONFIG_NCM_WRAP_COPY_ELIDE 0
#endif

#ifndef CONFIG_NCM_WRAP_STATS
#define CONFIG_NCM_WRAP_STATS 0
#endif

#if CONFIG_NCM_WRAP_PROFILE

/* Packed offsets recovered from the customer usbh_cdc_ncm_hal.o ABI. */
#define NCM_HOST_USER_TX_NTB_OFFSET 43U
#define NCM_TX_NTB_CAPACITY       16384U
#define NCM_TX_PAYLOAD_OFFSET        28U

typedef struct ncm_wrap_profile_stats_s {
	uint32_t calls;
	uint32_t ok;
	uint32_t errors;
	uint32_t bytes;
	uint32_t result_len_match;
	uint32_t result_len_mismatch;
	uint32_t header_match;
	uint32_t header_mismatch;
	uint32_t output_null;
	uint32_t output_changed_during_call;
	uint32_t input_eq_payload;
	uint32_t input_overlaps_output;
	uint32_t input_align4;
	uint32_t input_align32;
	uint32_t output_align4;
	uint32_t output_align32;
	uint32_t capacity_overflow;
	uint32_t cycles;
	uint32_t cycles_max;
	uint32_t elide_prepared;
	uint32_t elide_cancelled;
	uint32_t elide_activated;
	uint32_t elide_memset_preserved;
	uint32_t elide_memcpy_skipped;
	uint32_t elide_success;
	uint32_t elide_fallback;
	uint32_t elide_bytes;
} ncm_wrap_profile_stats_t;

typedef struct ncm_wrap_profile_state_s {
	uintptr_t output;
	uint32_t generation;
	uint32_t publications;
	uint32_t pointer_changes;
	uint32_t payload_offset;
	uintptr_t armed_packet;
	uint32_t armed_length;
	uint32_t armed_generation;
	uint32_t armed_token;
	uint32_t next_token;
	volatile uint32_t active;
	uintptr_t active_output;
	uintptr_t active_packet;
	uint32_t active_length;
	uint32_t active_offset;
	volatile uint32_t active_memset_preserved;
	volatile uint32_t active_memcpy_skipped;
} ncm_wrap_profile_state_t;

static ncm_wrap_profile_stats_t ncm_wrap_live;
static ncm_wrap_profile_stats_t ncm_wrap_snapshot;
static ncm_wrap_profile_state_t ncm_wrap_state;

extern int __real_ncm_wrap_ntb(void *host_user, const void *packet,
	uint32_t packet_length);
extern void *__real_memset(void *dest, int value, size_t length);

static uintptr_t ncm_load_packed_pointer(const void *base, uint32_t offset)
{
	const uint8_t *bytes = (const uint8_t *)base + offset;

	/* The customer ABI is 32-bit little-endian and stores this pointer at an
	 * unaligned packed offset.  Byte loads avoid undefined unaligned C access. */
	return (uintptr_t)bytes[0] |
		((uintptr_t)bytes[1] << 8) |
		((uintptr_t)bytes[2] << 16) |
		((uintptr_t)bytes[3] << 24);
}

static uint16_t ncm_load_le16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static int ncm_header_matches(const uint8_t *output, uint32_t packet_length,
	int result)
{
	if (output == NULL || result <= 0 ||
	    (uint32_t)result != packet_length + NCM_TX_PAYLOAD_OFFSET) {
		return 0;
	}

	return output[0] == 'N' && output[1] == 'C' &&
		output[2] == 'M' && output[3] == 'H' &&
		ncm_load_le16(output + 4) == 12U &&
		ncm_load_le16(output + 8) == (uint16_t)result &&
		ncm_load_le16(output + 10) == 12U &&
		output[12] == 'N' && output[13] == 'C' &&
		output[14] == 'M' && output[15] == '0' &&
		ncm_load_le16(output + 16) == 16U &&
		ncm_load_le16(output + 18) == 0U &&
		ncm_load_le16(output + 20) == NCM_TX_PAYLOAD_OFFSET &&
		ncm_load_le16(output + 22) == (uint16_t)packet_length &&
		ncm_load_le16(output + 24) == 0U &&
		ncm_load_le16(output + 26) == 0U;
}

int ncm_wrap_copy_elide_prepare(uint32_t packet_length, void **payload,
	void **allocation_end, uint32_t *token)
{
#if CONFIG_NCM_WRAP_COPY_ELIDE
	uint32_t local_token = 0U;

	if (payload == NULL || allocation_end == NULL || token == NULL) {
		return 0;
	}
	*payload = NULL;
	*allocation_end = NULL;
	*token = 0U;

	taskENTER_CRITICAL();
	if (ncm_wrap_state.output != 0U &&
	    ncm_wrap_state.payload_offset >= NCM_TX_PAYLOAD_OFFSET &&
	    ncm_wrap_state.payload_offset + packet_length <=
		NCM_TX_NTB_CAPACITY &&
	    ncm_wrap_state.armed_token == 0U &&
	    ncm_wrap_state.active == 0U) {
		ncm_wrap_state.next_token++;
		if (ncm_wrap_state.next_token == 0U) {
			ncm_wrap_state.next_token = 1U;
		}
		local_token = ncm_wrap_state.next_token;
		ncm_wrap_state.armed_packet = ncm_wrap_state.output +
			ncm_wrap_state.payload_offset;
		ncm_wrap_state.armed_length = packet_length;
		ncm_wrap_state.armed_generation = ncm_wrap_state.generation;
		ncm_wrap_state.armed_token = local_token;
#if CONFIG_NCM_WRAP_STATS
		ncm_wrap_live.elide_prepared++;
#endif
	}
	if (local_token != 0U) {
		*payload = (void *)ncm_wrap_state.armed_packet;
		*allocation_end = (void *)(ncm_wrap_state.output +
			NCM_TX_NTB_CAPACITY);
		*token = local_token;
	}
	taskEXIT_CRITICAL();
	return local_token != 0U;
#else
	(void)packet_length;
	(void)payload;
	(void)allocation_end;
	(void)token;
	return 0;
#endif
}

void ncm_wrap_copy_elide_cancel(uint32_t token)
{
#if CONFIG_NCM_WRAP_COPY_ELIDE
	if (token == 0U) {
		return;
	}
	taskENTER_CRITICAL();
	if (ncm_wrap_state.armed_token == token) {
		ncm_wrap_state.armed_token = 0U;
		ncm_wrap_state.armed_packet = 0U;
		ncm_wrap_state.armed_length = 0U;
#if CONFIG_NCM_WRAP_STATS
		ncm_wrap_live.elide_cancelled++;
#endif
	}
	taskEXIT_CRITICAL();
#else
	(void)token;
#endif
}

int ncm_wrap_copy_elide_memset(void *dst, int value, size_t length)
{
#if CONFIG_NCM_WRAP_COPY_ELIDE
	uintptr_t output = ncm_wrap_state.active_output;
	uint32_t offset = ncm_wrap_state.active_offset;
	uint32_t payload_length = ncm_wrap_state.active_length;
	size_t payload_end = (size_t)offset + payload_length;

	if (ncm_wrap_state.active == 0U || value != 0 ||
	    (uintptr_t)dst != output || length > NCM_TX_NTB_CAPACITY ||
	    offset == 0U || payload_end > length) {
		return 0;
	}
	__real_memset(dst, 0, offset);
	if (length > payload_end) {
		__real_memset((uint8_t *)dst + payload_end, 0,
			length - payload_end);
	}
	ncm_wrap_state.active_memset_preserved = 1U;
	return 1;
#else
	(void)dst;
	(void)value;
	(void)length;
	return 0;
#endif
}

int ncm_wrap_copy_elide_memcpy(void *dst, const void *src, size_t length)
{
#if CONFIG_NCM_WRAP_COPY_ELIDE
	if (ncm_wrap_state.active != 0U &&
	    (uintptr_t)dst == ncm_wrap_state.active_packet &&
	    (uintptr_t)src == ncm_wrap_state.active_packet &&
	    length == ncm_wrap_state.active_length &&
	    ncm_wrap_state.active_memset_preserved != 0U) {
		ncm_wrap_state.active_memcpy_skipped = 1U;
		return 1;
	}
#else
	(void)dst;
	(void)src;
	(void)length;
#endif
	return 0;
}

int __wrap_ncm_wrap_ntb(void *host_user, const void *packet,
	uint32_t packet_length)
{
#if CONFIG_NCM_WRAP_STATS
	ncm_wrap_profile_stats_t sample;
#endif
	uintptr_t output_before = 0U;
	uintptr_t output_after = 0U;
#if CONFIG_NCM_WRAP_STATS
	uint32_t start_cycles;
	uint32_t elapsed;
#endif
	uint32_t elide_active = 0U;
	int result;

#if CONFIG_NCM_WRAP_STATS
	memset(&sample, 0, sizeof(sample));
#endif
	if (host_user != NULL) {
		output_before = ncm_load_packed_pointer(
			host_user, NCM_HOST_USER_TX_NTB_OFFSET);
	}

#if CONFIG_NCM_WRAP_COPY_ELIDE
	taskENTER_CRITICAL();
	if (ncm_wrap_state.armed_token != 0U &&
	    ncm_wrap_state.armed_packet == (uintptr_t)packet &&
	    ncm_wrap_state.armed_length == packet_length &&
	    ncm_wrap_state.armed_generation == ncm_wrap_state.generation &&
	    ncm_wrap_state.output == output_before) {
		ncm_wrap_state.active_output = output_before;
		ncm_wrap_state.active_packet = (uintptr_t)packet;
		ncm_wrap_state.active_length = packet_length;
		ncm_wrap_state.active_offset = ncm_wrap_state.payload_offset;
		ncm_wrap_state.active_memset_preserved = 0U;
		ncm_wrap_state.active_memcpy_skipped = 0U;
		ncm_wrap_state.active = 1U;
		ncm_wrap_state.armed_token = 0U;
		elide_active = 1U;
#if CONFIG_NCM_WRAP_STATS
		sample.elide_activated = 1U;
#endif
	} else if (ncm_wrap_state.armed_token != 0U) {
		ncm_wrap_state.armed_token = 0U;
#if CONFIG_NCM_WRAP_STATS
		sample.elide_fallback = 1U;
#endif
	}
	taskEXIT_CRITICAL();
#endif

#if CONFIG_NCM_WRAP_STATS
	start_cycles = DWT->CYCCNT;
#endif
	result = __real_ncm_wrap_ntb(host_user, packet, packet_length);
#if CONFIG_NCM_WRAP_STATS
	elapsed = DWT->CYCCNT - start_cycles;
#endif

#if CONFIG_NCM_WRAP_COPY_ELIDE
	if (elide_active != 0U) {
#if CONFIG_NCM_WRAP_STATS
		sample.elide_memset_preserved =
			ncm_wrap_state.active_memset_preserved;
		sample.elide_memcpy_skipped =
			ncm_wrap_state.active_memcpy_skipped;
		if (sample.elide_memset_preserved != 0U &&
		    sample.elide_memcpy_skipped != 0U) {
			sample.elide_success = 1U;
			sample.elide_bytes = packet_length;
		} else {
			sample.elide_fallback = 1U;
		}
#endif
		ncm_wrap_state.active = 0U;
		ncm_wrap_state.active_output = 0U;
		ncm_wrap_state.active_packet = 0U;
		ncm_wrap_state.active_length = 0U;
		ncm_wrap_state.active_offset = 0U;
	}
#endif

	if (host_user != NULL) {
		output_after = ncm_load_packed_pointer(
			host_user, NCM_HOST_USER_TX_NTB_OFFSET);
	}

#if CONFIG_NCM_WRAP_STATS
	sample.calls = 1U;
	sample.bytes = packet_length;
	sample.cycles = elapsed;
	sample.cycles_max = elapsed;
	if (result > 0) {
		sample.ok = 1U;
	} else {
		sample.errors = 1U;
	}
	if (result > 0 &&
	    (uint32_t)result == packet_length + NCM_TX_PAYLOAD_OFFSET) {
		sample.result_len_match = 1U;
	} else {
		sample.result_len_mismatch = 1U;
	}
	if (output_after == 0U) {
		sample.output_null = 1U;
	} else {
		if ((output_after & 3U) == 0U) {
			sample.output_align4 = 1U;
		}
		if ((output_after & 31U) == 0U) {
			sample.output_align32 = 1U;
		}
		if (ncm_header_matches((const uint8_t *)output_after,
				       packet_length, result)) {
			sample.header_match = 1U;
		} else {
			sample.header_mismatch = 1U;
		}
	}
	if (output_before != output_after) {
		sample.output_changed_during_call = 1U;
	}
	if (packet != NULL) {
		uintptr_t input = (uintptr_t)packet;

		if ((input & 3U) == 0U) {
			sample.input_align4 = 1U;
		}
		if ((input & 31U) == 0U) {
			sample.input_align32 = 1U;
		}
		if (output_after != 0U &&
		    input == output_after + NCM_TX_PAYLOAD_OFFSET) {
			sample.input_eq_payload = 1U;
		}
		if (output_after != 0U &&
		    input < output_after + NCM_TX_NTB_CAPACITY &&
		    input + packet_length > output_after) {
			sample.input_overlaps_output = 1U;
		}
	}
	if (packet_length + NCM_TX_PAYLOAD_OFFSET > NCM_TX_NTB_CAPACITY) {
		sample.capacity_overflow = 1U;
	}

	taskENTER_CRITICAL();
	ncm_wrap_live.calls += sample.calls;
	ncm_wrap_live.ok += sample.ok;
	ncm_wrap_live.errors += sample.errors;
	ncm_wrap_live.bytes += sample.bytes;
	ncm_wrap_live.result_len_match += sample.result_len_match;
	ncm_wrap_live.result_len_mismatch += sample.result_len_mismatch;
	ncm_wrap_live.header_match += sample.header_match;
	ncm_wrap_live.header_mismatch += sample.header_mismatch;
	ncm_wrap_live.output_null += sample.output_null;
	ncm_wrap_live.output_changed_during_call +=
		sample.output_changed_during_call;
	ncm_wrap_live.input_eq_payload += sample.input_eq_payload;
	ncm_wrap_live.input_overlaps_output += sample.input_overlaps_output;
	ncm_wrap_live.input_align4 += sample.input_align4;
	ncm_wrap_live.input_align32 += sample.input_align32;
	ncm_wrap_live.output_align4 += sample.output_align4;
	ncm_wrap_live.output_align32 += sample.output_align32;
	ncm_wrap_live.capacity_overflow += sample.capacity_overflow;
	ncm_wrap_live.cycles += sample.cycles;
	ncm_wrap_live.elide_activated += sample.elide_activated;
	ncm_wrap_live.elide_memset_preserved += sample.elide_memset_preserved;
	ncm_wrap_live.elide_memcpy_skipped += sample.elide_memcpy_skipped;
	ncm_wrap_live.elide_success += sample.elide_success;
	ncm_wrap_live.elide_fallback += sample.elide_fallback;
	ncm_wrap_live.elide_bytes += sample.elide_bytes;
	if (sample.cycles_max > ncm_wrap_live.cycles_max) {
		ncm_wrap_live.cycles_max = sample.cycles_max;
	}
	taskEXIT_CRITICAL();
#endif

	taskENTER_CRITICAL();
	if (output_after != 0U && ncm_wrap_state.output != output_after) {
		if (ncm_wrap_state.output != 0U) {
			ncm_wrap_state.pointer_changes++;
		}
		ncm_wrap_state.output = output_after;
		ncm_wrap_state.generation++;
		if (ncm_wrap_state.generation == 0U) {
			ncm_wrap_state.generation = 1U;
		}
		ncm_wrap_state.publications++;
	}
	if (output_after != 0U &&
	    ncm_header_matches((const uint8_t *)output_after,
		packet_length, result)) {
		ncm_wrap_state.payload_offset =
			ncm_load_le16((const uint8_t *)output_after + 20U);
	}
	taskEXIT_CRITICAL();

	return result;
}

void ncm_wrap_profiler_report(uint32_t sequence)
{
#if CONFIG_NCM_WRAP_STATS
	ncm_wrap_profile_state_t state;
	uint32_t cycles_per_us = SystemCoreClock / 1000000U;
	uint32_t avg_us;
	uint32_t max_us;

	taskENTER_CRITICAL();
	ncm_wrap_snapshot = ncm_wrap_live;
	memset(&ncm_wrap_live, 0, sizeof(ncm_wrap_live));
	state = ncm_wrap_state;
	taskEXIT_CRITICAL();

	avg_us = ncm_wrap_snapshot.calls != 0U && cycles_per_us != 0U ?
		ncm_wrap_snapshot.cycles / cycles_per_us /
		ncm_wrap_snapshot.calls : 0U;
	max_us = cycles_per_us != 0U ?
		ncm_wrap_snapshot.cycles_max / cycles_per_us : 0U;

	rt_printf("[NCMWRAP][%lu] calls/ok/error=%lu/%lu/%lu bytes=%lu "
		  "time_us avg/max=%lu/%lu result_len match/mismatch=%lu/%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)ncm_wrap_snapshot.calls,
		  (unsigned long)ncm_wrap_snapshot.ok,
		  (unsigned long)ncm_wrap_snapshot.errors,
		  (unsigned long)ncm_wrap_snapshot.bytes,
		  (unsigned long)avg_us,
		  (unsigned long)max_us,
		  (unsigned long)ncm_wrap_snapshot.result_len_match,
		  (unsigned long)ncm_wrap_snapshot.result_len_mismatch);
	rt_printf("[NCMWRAP][%lu] header match/mismatch=%lu/%lu output "
		  "null/changed=%lu/%lu align4/32=%lu/%lu input align4/32=%lu/%lu "
		  "same_payload/overlap=%lu/%lu capacity_over=%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)ncm_wrap_snapshot.header_match,
		  (unsigned long)ncm_wrap_snapshot.header_mismatch,
		  (unsigned long)ncm_wrap_snapshot.output_null,
		  (unsigned long)ncm_wrap_snapshot.output_changed_during_call,
		  (unsigned long)ncm_wrap_snapshot.output_align4,
		  (unsigned long)ncm_wrap_snapshot.output_align32,
		  (unsigned long)ncm_wrap_snapshot.input_align4,
		  (unsigned long)ncm_wrap_snapshot.input_align32,
		  (unsigned long)ncm_wrap_snapshot.input_eq_payload,
		  (unsigned long)ncm_wrap_snapshot.input_overlaps_output,
		  (unsigned long)ncm_wrap_snapshot.capacity_overflow);
	rt_printf("[NCMWRAP][%lu] output=0x%08lx generation/publications/changes="
		  "%lu/%lu/%lu builder_forwarded=1 copy_elide=%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)state.output,
		  (unsigned long)state.generation,
		  (unsigned long)state.publications,
		  (unsigned long)state.pointer_changes,
		  (unsigned long)CONFIG_NCM_WRAP_COPY_ELIDE);
	rt_printf("[NCMELIDE][%lu] prepare/cancel/activate=%lu/%lu/%lu "
		  "preserve/skip/success/fallback=%lu/%lu/%lu/%lu bytes_saved=%lu "
		  "payload_offset=%lu validated=1\r\n",
		  (unsigned long)sequence,
		  (unsigned long)ncm_wrap_snapshot.elide_prepared,
		  (unsigned long)ncm_wrap_snapshot.elide_cancelled,
		  (unsigned long)ncm_wrap_snapshot.elide_activated,
		  (unsigned long)ncm_wrap_snapshot.elide_memset_preserved,
		  (unsigned long)ncm_wrap_snapshot.elide_memcpy_skipped,
		  (unsigned long)ncm_wrap_snapshot.elide_success,
		  (unsigned long)ncm_wrap_snapshot.elide_fallback,
		  (unsigned long)ncm_wrap_snapshot.elide_bytes,
		  (unsigned long)state.payload_offset);
#else
	(void)sequence;
#endif
}

#else

void ncm_wrap_profiler_report(uint32_t sequence)
{
	(void)sequence;
}

int ncm_wrap_copy_elide_prepare(uint32_t packet_length, void **payload,
	void **allocation_end, uint32_t *token)
{
	(void)packet_length;
	(void)payload;
	(void)allocation_end;
	(void)token;
	return 0;
}

void ncm_wrap_copy_elide_cancel(uint32_t token)
{
	(void)token;
}

int ncm_wrap_copy_elide_memset(void *dst, int value, size_t length)
{
	(void)dst;
	(void)value;
	(void)length;
	return 0;
}

int ncm_wrap_copy_elide_memcpy(void *dst, const void *src, size_t length)
{
	(void)dst;
	(void)src;
	(void)length;
	return 0;
}

#endif
