#include "screen_tx_direct_crypto.h"
#include "usb_hcd_profiler.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "diag.h"
#include "hal_timer.h"
#include "cmsis.h"
#include "lwip_intf.h"
#include "lwip/sockets.h"

#ifndef CONFIG_SCREEN_TX_DIRECT_CRYPTO
#define CONFIG_SCREEN_TX_DIRECT_CRYPTO 0
#endif

#ifndef SCREEN_TX_DIRECT_CRYPTO_MIN_BYTES
#define SCREEN_TX_DIRECT_CRYPTO_MIN_BYTES 4096U
#endif
#ifndef CONFIG_TCP_OWNED_WRITE
#define CONFIG_TCP_OWNED_WRITE 0
#endif
#ifndef CONFIG_SCREEN_BLOCK_PROFILE
#define CONFIG_SCREEN_BLOCK_PROFILE 0
#endif
#ifndef CONFIG_SCREEN_USB_PROBE
#define CONFIG_SCREEN_USB_PROBE 0
#endif
#ifndef CONFIG_SCREEN_TX_PACER
#define CONFIG_SCREEN_TX_PACER 0
#endif
#ifndef CONFIG_SCREEN_TX_PACER_BPS
#define CONFIG_SCREEN_TX_PACER_BPS 8000000U
#endif
#ifndef CONFIG_SCREEN_TX_PACER_BUCKET_BYTES
#define CONFIG_SCREEN_TX_PACER_BUCKET_BYTES 23040U
#endif
#ifndef CONFIG_SCREEN_TX_PACER_CHUNK_BYTES
#define CONFIG_SCREEN_TX_PACER_CHUNK_BYTES 4096U
#endif
#ifndef CONFIG_SCREEN_TX_PACER_WAIT_MS
#define CONFIG_SCREEN_TX_PACER_WAIT_MS 1U
#endif
#ifndef CONFIG_SCREEN_TX_PRESSURE_FEEDBACK
#define CONFIG_SCREEN_TX_PRESSURE_FEEDBACK 0
#endif
#ifndef CONFIG_SCREEN_RX_RATE_LIMIT
#define CONFIG_SCREEN_RX_RATE_LIMIT 0
#endif
#ifndef CONFIG_SCREEN_TX_PRESSURE_TRIGGER_MS
#define CONFIG_SCREEN_TX_PRESSURE_TRIGGER_MS 75U
#endif

#if CONFIG_SCREEN_TX_PACER && \
	((CONFIG_SCREEN_TX_PACER_BPS == 0) || \
	 (CONFIG_SCREEN_TX_PACER_BUCKET_BYTES == 0) || \
	 (CONFIG_SCREEN_TX_PACER_CHUNK_BYTES == 0) || \
	 (CONFIG_SCREEN_TX_PACER_CHUNK_BYTES > \
	  CONFIG_SCREEN_TX_PACER_BUCKET_BYTES) || \
	 (CONFIG_SCREEN_TX_PACER_WAIT_MS == 0))
#error "invalid screen TX pacer configuration"
#endif

#if CONFIG_SCREEN_TX_DIRECT_CRYPTO

#define SCREEN_TX_DIRECT_SLOTS 8U
#define SCREEN_TX_HEADER_BYTES 128U
#define SCREEN_TX_TAG_BYTES    16U

typedef enum screen_tx_direct_state_e {
	SCREEN_TX_DIRECT_EMPTY = 0,
	SCREEN_TX_DIRECT_CANDIDATE,
	SCREEN_TX_DIRECT_HEADER,
	SCREEN_TX_DIRECT_DEFERRED,
	SCREEN_TX_DIRECT_ACTIVE,
	SCREEN_TX_DIRECT_READY,
	SCREEN_TX_DIRECT_FAILED
} screen_tx_direct_state_t;

typedef struct screen_tx_direct_slot_s {
	TaskHandle_t owner;
	void *wire_base;
	size_t wire_length;
	const void *source;
	void *destination;
	size_t payload_length;
	uint32_t crypto_kind;
	uint8_t state;
	uint8_t materialized;
	uint8_t tcp_owned;
	uint8_t consumer_released;
	uint8_t network_released;
	uint16_t network_refs;
} screen_tx_direct_slot_t;

typedef struct screen_tx_direct_stats_s {
	uint32_t allocations;
	uint32_t headers;
	uint32_t attempts;
	uint32_t deferred;
	uint32_t aes_success;
	uint32_t chacha_success;
	uint32_t fallbacks;
	uint32_t failures;
	uint32_t stale_write;
	uint32_t releases;
	uint32_t table_full;
	uint32_t prefix_same;
	uint32_t prefix_rewritten;
	uint32_t owned_begin;
	uint32_t owned_deferred_free;
	uint32_t owned_completions;
	uint32_t owned_final_frees;
	uint64_t bytes_saved;
} screen_tx_direct_stats_t;

static screen_tx_direct_slot_t screen_tx_slots[SCREEN_TX_DIRECT_SLOTS];
static screen_tx_direct_stats_t screen_tx_stats;
static uint8_t screen_tx_enabled = 1U;

#if CONFIG_SCREEN_TX_PACER
typedef struct screen_tx_pacer_stats_s {
	uint32_t calls;
	uint32_t partials;
	uint32_t waits;
	uint32_t wait_max_us;
	uint64_t wait_sum_us;
	uint64_t requested_bytes;
	uint64_t allowed_bytes;
	uint64_t written_bytes;
} screen_tx_pacer_stats_t;

static TaskHandle_t screen_tx_pacer_task;
static uint32_t screen_tx_pacer_tokens = CONFIG_SCREEN_TX_PACER_BUCKET_BYTES;
static uint32_t screen_tx_pacer_last_us;
static uint32_t screen_tx_pacer_remainder;
static screen_tx_pacer_stats_t screen_tx_pacer_stats;

static int screen_tx_pacer_is_sender(void)
{
	TaskHandle_t current = xTaskGetCurrentTaskHandle();

	if (current == screen_tx_pacer_task) {
		return 1;
	}
	if (strcmp(pcTaskGetTaskName(current), "ScreenThread") == 0) {
		screen_tx_pacer_task = current;
		screen_tx_pacer_last_us = hal_read_curtime_us();
		screen_tx_pacer_tokens = CONFIG_SCREEN_TX_PACER_BUCKET_BYTES;
		screen_tx_pacer_remainder = 0U;
		return 1;
	}
	return 0;
}

static void screen_tx_pacer_refill_locked(uint32_t now_us)
{
	uint32_t elapsed_us = now_us - screen_tx_pacer_last_us;
	uint64_t numerator;
	uint32_t refill;

	screen_tx_pacer_last_us = now_us;
	numerator = (uint64_t)CONFIG_SCREEN_TX_PACER_BPS * elapsed_us +
		screen_tx_pacer_remainder;
	refill = (uint32_t)(numerator / 8000000U);
	screen_tx_pacer_remainder = (uint32_t)(numerator % 8000000U);
	if (refill >= CONFIG_SCREEN_TX_PACER_BUCKET_BYTES -
			screen_tx_pacer_tokens) {
		screen_tx_pacer_tokens = CONFIG_SCREEN_TX_PACER_BUCKET_BYTES;
	} else {
		screen_tx_pacer_tokens += refill;
	}
}

size_t carbox_screen_tx_pacer_allowance(size_t requested)
{
	size_t chunk;
	uint32_t wait_start_us;
	uint32_t waited_us;
	uint32_t wait_loops = 0U;

	if ((requested == 0U) || !screen_tx_pacer_is_sender()) {
		return requested;
	}
	chunk = requested < CONFIG_SCREEN_TX_PACER_CHUNK_BYTES ? requested :
		CONFIG_SCREEN_TX_PACER_CHUNK_BYTES;
	wait_start_us = hal_read_curtime_us();
	for (;;) {
		uint32_t now_us = hal_read_curtime_us();

		taskENTER_CRITICAL();
		screen_tx_pacer_refill_locked(now_us);
		if (screen_tx_pacer_tokens >= chunk) {
			screen_tx_pacer_tokens -= (uint32_t)chunk;
			screen_tx_pacer_stats.calls++;
			screen_tx_pacer_stats.requested_bytes += requested;
			screen_tx_pacer_stats.allowed_bytes += chunk;
			screen_tx_pacer_stats.partials += chunk < requested;
			waited_us = now_us - wait_start_us;
			if (wait_loops != 0U) {
				screen_tx_pacer_stats.waits++;
				screen_tx_pacer_stats.wait_sum_us += waited_us;
				if (waited_us > screen_tx_pacer_stats.wait_max_us) {
					screen_tx_pacer_stats.wait_max_us = waited_us;
				}
			}
			taskEXIT_CRITICAL();
			return chunk;
		}
		taskEXIT_CRITICAL();
		wait_loops++;
		vTaskDelay(pdMS_TO_TICKS(CONFIG_SCREEN_TX_PACER_WAIT_MS));
	}
}

void carbox_screen_tx_pacer_complete(size_t allowed, int result)
{
	uint32_t unused = result > 0 && (size_t)result < allowed ?
		(uint32_t)(allowed - (size_t)result) :
		(result < 0 ? (uint32_t)allowed : 0U);

	if (!screen_tx_pacer_is_sender()) {
		return;
	}
	taskENTER_CRITICAL();
	if (unused >= CONFIG_SCREEN_TX_PACER_BUCKET_BYTES -
			screen_tx_pacer_tokens) {
		screen_tx_pacer_tokens = CONFIG_SCREEN_TX_PACER_BUCKET_BYTES;
	} else {
		screen_tx_pacer_tokens += unused;
	}
	if (result > 0) {
		screen_tx_pacer_stats.written_bytes += (uint32_t)result;
	}
	taskEXIT_CRITICAL();
}
#else
size_t carbox_screen_tx_pacer_allowance(size_t requested)
{
	return requested;
}

void carbox_screen_tx_pacer_complete(size_t allowed, int result)
{
	(void)allowed;
	(void)result;
}
#endif

#if CONFIG_SCREEN_BLOCK_PROFILE || CONFIG_SCREEN_USB_PROBE
typedef struct screen_block_stats_s {
	uint32_t writes;
	uint32_t successful;
	uint32_t partial;
	uint32_t eagain;
	uint32_t other_error;
	int32_t last_errno;
	uint32_t probe_error;
	uint32_t tx_buffer_capacity;
	uint32_t tx_buffer_min;
	uint32_t tx_queue_capacity;
	uint32_t tx_queue_max;
	uint32_t unsent_bytes_max;
	uint32_t unsent_segments_max;
	uint32_t unacked_bytes_max;
	uint32_t unacked_segments_max;
	uint32_t send_window_min;
	uint32_t send_window_available_min;
	uint32_t congestion_window_min;
	uint32_t buffer_empty;
	uint32_t queue_full;
	uint32_t send_window_limited;
	uint32_t ncm_queue_max;
	uint32_t ncm_queue_capacity;
	uint32_t ncm_inflight_max;
	uint32_t ncm_inflight_age_max_us;
	uint32_t block_episodes;
	uint32_t block_recoveries;
	uint32_t block_retries;
	uint32_t recovery_max_us;
	uint64_t recovery_sum_us;
} screen_block_stats_t;

static screen_block_stats_t screen_block_stats = {
	.tx_buffer_min = UINT32_MAX,
	.send_window_min = UINT32_MAX,
	.send_window_available_min = UINT32_MAX,
	.congestion_window_min = UINT32_MAX,
};
static TaskHandle_t screen_block_task;
static uint8_t screen_block_active;
static uint8_t screen_block_pressure_signaled;
static uint32_t screen_block_start_us;
static uint32_t screen_block_active_retries;

static int screen_block_is_sender(void)
{
	TaskHandle_t current = xTaskGetCurrentTaskHandle();

	if (current == screen_block_task) {
		return 1;
	}
	if (strcmp(pcTaskGetTaskName(current), "ScreenThread") == 0) {
		screen_block_task = current;
		return 1;
	}
	return 0;
}

static void screen_block_min(uint32_t *value, uint32_t sample)
{
	if ((*value == UINT32_MAX) || (sample < *value)) {
		*value = sample;
	}
}

static void screen_block_max(uint32_t *value, uint32_t sample)
{
	if (sample > *value) {
		*value = sample;
	}
}
#endif

static __attribute__((always_inline)) inline int screen_tx_in_isr(void)
{
	uint32_t ipsr;

	__asm volatile("mrs %0, ipsr" : "=r" (ipsr));
	return ipsr != 0U;
}

static screen_tx_direct_slot_t *screen_tx_find_task_locked(TaskHandle_t task)
{
	uint32_t i;

	for (i = 0U; i < SCREEN_TX_DIRECT_SLOTS; i++) {
		/* ACK-owned frames remain in the table after ScreenThread starts the
		 * next frame.  They are not the task's active crypto transaction. */
		if ((screen_tx_slots[i].owner == task) &&
		    !screen_tx_slots[i].tcp_owned) {
			return &screen_tx_slots[i];
		}
	}
	return NULL;
}

static screen_tx_direct_slot_t *screen_tx_find_destination_locked(
	TaskHandle_t task, const void *destination, size_t length)
{
	screen_tx_direct_slot_t *slot = screen_tx_find_task_locked(task);

	if ((slot != NULL) && (slot->destination == destination) &&
	    (slot->payload_length == length)) {
		return slot;
	}
	return NULL;
}

static void screen_tx_clear_slot_locked(screen_tx_direct_slot_t *slot)
{
	*slot = (screen_tx_direct_slot_t){ 0 };
}

static int screen_tx_wire_size_matches(size_t allocation, size_t payload)
{
	if (payload > SIZE_MAX - SCREEN_TX_HEADER_BYTES) {
		return 0;
	}
	if (allocation == payload + SCREEN_TX_HEADER_BYTES) {
		return 1;
	}
	if (payload > SIZE_MAX - SCREEN_TX_HEADER_BYTES - SCREEN_TX_TAG_BYTES) {
		return 0;
	}
	return allocation == payload + SCREEN_TX_HEADER_BYTES +
		SCREEN_TX_TAG_BYTES;
}

static void screen_tx_disable(const char *reason, const void *pointer)
{
	taskENTER_CRITICAL();
	screen_tx_enabled = 0U;
	screen_tx_stats.failures++;
	taskEXIT_CRITICAL();
	rt_printf("[TXCRYPTO_DIRECT][FATAL] reason=%s pointer=0x%08lx; "
		  "new direct transactions disabled\r\n", reason,
		  (unsigned long)(uintptr_t)pointer);
}

void carbox_screen_tx_allocation(void *pointer, size_t length)
{
	TaskHandle_t task;
	screen_tx_direct_slot_t *slot;
	uint32_t i;

	if ((pointer == NULL) ||
	    (length < SCREEN_TX_HEADER_BYTES +
		SCREEN_TX_DIRECT_CRYPTO_MIN_BYTES)) {
		return;
	}
	task = xTaskGetCurrentTaskHandle();
	taskENTER_CRITICAL();
	if (!screen_tx_enabled) {
		taskEXIT_CRITICAL();
		return;
	}
	slot = screen_tx_find_task_locked(task);
	if (slot == NULL) {
		for (i = 0U; i < SCREEN_TX_DIRECT_SLOTS; i++) {
			if (screen_tx_slots[i].state == SCREEN_TX_DIRECT_EMPTY) {
				slot = &screen_tx_slots[i];
				break;
			}
		}
	}
	if (slot == NULL) {
		screen_tx_stats.table_full++;
		taskEXIT_CRITICAL();
		return;
	}
	/* A new object-local allocation before the old candidate is used cannot
	 * be the normal sender sequence. Replace only an uncommitted candidate. */
	if ((slot->state != SCREEN_TX_DIRECT_EMPTY) &&
	    (slot->state != SCREEN_TX_DIRECT_CANDIDATE)) {
		taskEXIT_CRITICAL();
		return;
	}
	*slot = (screen_tx_direct_slot_t){
		.owner = task,
		.wire_base = pointer,
		.wire_length = length,
		.state = SCREEN_TX_DIRECT_CANDIDATE
	};
	screen_tx_stats.allocations++;
	taskEXIT_CRITICAL();
}

void carbox_screen_tx_release(void *pointer)
{
	TaskHandle_t task = xTaskGetCurrentTaskHandle();
	screen_tx_direct_slot_t *slot;
	int stale = 0;

	if (pointer == NULL) {
		return;
	}
	taskENTER_CRITICAL();
	slot = screen_tx_find_task_locked(task);
	if ((slot != NULL) && (slot->wire_base == pointer)) {
		stale = (slot->state == SCREEN_TX_DIRECT_DEFERRED) ||
			(slot->state == SCREEN_TX_DIRECT_ACTIVE);
		screen_tx_clear_slot_locked(slot);
		screen_tx_stats.releases++;
	}
	taskEXIT_CRITICAL();
	if (stale) {
		screen_tx_disable("release-before-crypto-complete", pointer);
	}
}

int carbox_screen_tx_owned_begin(const void *pointer, size_t length)
{
	TaskHandle_t task = xTaskGetCurrentTaskHandle();
	screen_tx_direct_slot_t *slot = NULL;
	uintptr_t address = (uintptr_t)pointer;
	uint32_t i;
	int owned = 0;

	taskENTER_CRITICAL();
	for (i = 0U; i < SCREEN_TX_DIRECT_SLOTS; i++) {
		uintptr_t base = (uintptr_t)screen_tx_slots[i].wire_base;

		if ((screen_tx_slots[i].owner == task) &&
		    (screen_tx_slots[i].state == SCREEN_TX_DIRECT_READY) &&
		    (address >= base) &&
		    (address - base <= screen_tx_slots[i].wire_length) &&
		    (length <= screen_tx_slots[i].wire_length -
			(address - base))) {
			slot = &screen_tx_slots[i];
			break;
		}
	}
	if ((slot != NULL) && (slot->network_refs != UINT16_MAX)) {
		if (!slot->tcp_owned) {
			slot->tcp_owned = 1U;
			slot->consumer_released = 0U;
		}
		slot->network_released = 0U;
		slot->network_refs++;
		screen_tx_stats.owned_begin++;
		owned = 1;
	}
	taskEXIT_CRITICAL();
	return owned;
}

int carbox_screen_tx_owned_consumer_release(void *pointer)
{
	uint32_t i;
	int defer = 0;

	taskENTER_CRITICAL();
	for (i = 0U; i < SCREEN_TX_DIRECT_SLOTS; i++) {
		screen_tx_direct_slot_t *slot = &screen_tx_slots[i];

		if ((slot->wire_base == pointer) && slot->tcp_owned) {
			if (slot->network_refs == 0U) {
				screen_tx_clear_slot_locked(slot);
			} else {
				slot->consumer_released = 1U;
				screen_tx_stats.owned_deferred_free++;
				defer = 1;
			}
			break;
		}
	}
	taskEXIT_CRITICAL();
	return defer;
}

void carbox_screen_tx_owned_complete(void *pointer)
{
	uint32_t i;
	int final_free = 0;
	void *final_pointer = NULL;
	uintptr_t address = (uintptr_t)pointer;

	taskENTER_CRITICAL();
	for (i = 0U; i < SCREEN_TX_DIRECT_SLOTS; i++) {
		screen_tx_direct_slot_t *slot = &screen_tx_slots[i];
		uintptr_t base = (uintptr_t)slot->wire_base;

		if (slot->tcp_owned && (address >= base) &&
		    (address - base < slot->wire_length)) {
			if (slot->network_refs != 0U) {
				slot->network_refs--;
			}
			slot->network_released = slot->network_refs == 0U;
			screen_tx_stats.owned_completions++;
			if (slot->consumer_released &&
			    (slot->network_refs == 0U)) {
				final_pointer = slot->wire_base;
				screen_tx_clear_slot_locked(slot);
				screen_tx_stats.owned_final_frees++;
				final_free = 1;
			}
			break;
		}
	}
	taskEXIT_CRITICAL();
	if (final_free) {
		free(final_pointer);
	}
}

static int screen_tx_copy_defer(void *destination, const void *source,
				 size_t length)
{
	TaskHandle_t task = xTaskGetCurrentTaskHandle();
	screen_tx_direct_slot_t *slot;
	int defer = 0;

	taskENTER_CRITICAL();
	slot = screen_tx_find_task_locked(task);
	if ((slot != NULL) && screen_tx_enabled) {
		if ((slot->state == SCREEN_TX_DIRECT_CANDIDATE) &&
		    (destination == slot->wire_base) &&
		    (length == SCREEN_TX_HEADER_BYTES)) {
			slot->state = SCREEN_TX_DIRECT_HEADER;
			screen_tx_stats.headers++;
		} else if ((slot->state == SCREEN_TX_DIRECT_HEADER) &&
			   (destination == (void *)((uintptr_t)slot->wire_base +
				SCREEN_TX_HEADER_BYTES)) &&
			   (source != NULL) && (destination != source) &&
			   (length >= SCREEN_TX_DIRECT_CRYPTO_MIN_BYTES) &&
			   screen_tx_wire_size_matches(slot->wire_length, length)) {
			slot->source = source;
			slot->destination = destination;
			slot->payload_length = length;
			slot->state = SCREEN_TX_DIRECT_DEFERRED;
			screen_tx_stats.attempts++;
			screen_tx_stats.deferred++;
			defer = 1;
		}
	}
	taskEXIT_CRITICAL();
	return defer;
}

void *carbox_airplay_screen_memcpy(void *destination, const void *source,
				   size_t length)
{
	if (screen_tx_copy_defer(destination, source, length)) {
		return destination;
	}
	return memcpy(destination, source, length);
}

int carbox_screen_tx_crypto_begin(void *alias_source, size_t length,
				  void *destination, uint32_t kind,
				  const void **direct_source)
{
	TaskHandle_t task = xTaskGetCurrentTaskHandle();
	screen_tx_direct_slot_t *slot;
	const uint8_t *prefix;
	uint8_t *source;
	int match = 0;

	if (direct_source != NULL) {
		*direct_source = alias_source;
	}
	/* These APIs are linked globally, but direct-screen transaction state is
	 * task-owned and protected by taskENTER_CRITICAL().  An ISR must retain the
	 * original crypto operation without inspecting that state. */
	if (screen_tx_in_isr()) {
		return 0;
	}
	taskENTER_CRITICAL();
	slot = screen_tx_find_destination_locked(task, destination, length);
	if ((slot != NULL) && screen_tx_enabled &&
	    (slot->state == SCREEN_TX_DIRECT_DEFERRED) &&
	    (alias_source == destination) &&
	    ((kind == CARBOX_SCREEN_TX_CRYPTO_AES) ||
	     (kind == CARBOX_SCREEN_TX_CRYPTO_CHACHA))) {
		source = (uint8_t *)(uintptr_t)slot->source;
		prefix = (const uint8_t *)destination;
		if ((source[0] == prefix[0]) && (source[1] == prefix[1]) &&
		    (source[2] == prefix[2]) && (source[3] == prefix[3])) {
			screen_tx_stats.prefix_same++;
		} else {
			source[0] = prefix[0];
			source[1] = prefix[1];
			source[2] = prefix[2];
			source[3] = prefix[3];
			screen_tx_stats.prefix_rewritten++;
		}
		slot->crypto_kind = kind;
		slot->state = SCREEN_TX_DIRECT_ACTIVE;
		if (direct_source != NULL) {
			*direct_source = slot->source;
		}
		match = 1;
	}
	taskEXIT_CRITICAL();
	return match;
}

int carbox_screen_tx_crypto_active(void *destination, size_t length,
				   const void **direct_source)
{
	TaskHandle_t task = xTaskGetCurrentTaskHandle();
	screen_tx_direct_slot_t *slot;
	int active = 0;

	if (direct_source != NULL) {
		*direct_source = destination;
	}
	if (screen_tx_in_isr()) {
		return 0;
	}
	taskENTER_CRITICAL();
	slot = screen_tx_find_destination_locked(task, destination, length);
	if ((slot != NULL) && (slot->state == SCREEN_TX_DIRECT_ACTIVE) &&
	    (slot->crypto_kind == CARBOX_SCREEN_TX_CRYPTO_CHACHA)) {
		if (direct_source != NULL) {
			*direct_source = slot->source;
		}
		active = 1;
	}
	taskEXIT_CRITICAL();
	return active;
}

void carbox_screen_tx_crypto_materialized(void *destination, size_t length)
{
	TaskHandle_t task = xTaskGetCurrentTaskHandle();
	screen_tx_direct_slot_t *slot;

	taskENTER_CRITICAL();
	slot = screen_tx_find_destination_locked(task, destination, length);
	if ((slot != NULL) && (slot->state == SCREEN_TX_DIRECT_ACTIVE)) {
		slot->materialized = 1U;
	}
	screen_tx_stats.fallbacks++;
	taskEXIT_CRITICAL();
}

void carbox_screen_tx_crypto_complete(void *destination, size_t length,
				      uint32_t kind, int status)
{
	TaskHandle_t task = xTaskGetCurrentTaskHandle();
	screen_tx_direct_slot_t *slot;
	int fatal = 0;

	taskENTER_CRITICAL();
	slot = screen_tx_find_destination_locked(task, destination, length);
	if ((slot != NULL) && (slot->state == SCREEN_TX_DIRECT_ACTIVE) &&
	    (slot->crypto_kind == kind)) {
		if (status == 0) {
			slot->state = SCREEN_TX_DIRECT_READY;
			if (!slot->materialized) {
				screen_tx_stats.bytes_saved += length;
			}
			if (kind == CARBOX_SCREEN_TX_CRYPTO_AES) {
				screen_tx_stats.aes_success++;
			} else {
				screen_tx_stats.chacha_success++;
			}
		} else {
			slot->state = SCREEN_TX_DIRECT_FAILED;
			screen_tx_stats.failures++;
			fatal = 1;
		}
	}
	taskEXIT_CRITICAL();
	if (fatal) {
		rt_printf("[TXCRYPTO_DIRECT][FATAL] kind=%lu status=%d "
			  "dst=0x%08lx len=%lu\r\n", (unsigned long)kind,
			  status, (unsigned long)(uintptr_t)destination,
			  (unsigned long)length);
	}
}

void carbox_screen_tx_before_write(const void *buffer, size_t length)
{
	TaskHandle_t task = xTaskGetCurrentTaskHandle();
	screen_tx_direct_slot_t *slot;
	int stale = 0;

	(void)length;
	taskENTER_CRITICAL();
	slot = screen_tx_find_task_locked(task);
	if ((slot != NULL) && (slot->wire_base == buffer) &&
	    ((slot->state == SCREEN_TX_DIRECT_DEFERRED) ||
	     (slot->state == SCREEN_TX_DIRECT_ACTIVE) ||
	     (slot->state == SCREEN_TX_DIRECT_FAILED))) {
		screen_tx_stats.stale_write++;
		stale = 1;
	}
	taskEXIT_CRITICAL();
	if (stale) {
		screen_tx_disable("wire-write-before-valid-crypto", buffer);
	}
}

void carbox_screen_block_profile_write(int socket, size_t requested, int result)
{
#if CONFIG_SCREEN_BLOCK_PROFILE || CONFIG_SCREEN_USB_PROBE
	struct lwip_tcp_buffer_diag tcp;
	rltk_ncm_tx_diag_t ncm;
	/* sock_set_errno() updates the lwIP global errno.  Realtek's
	 * lwip_getsocklasterr() returns sock->errevent (an event count), not errno,
	 * and also fails to release the socket reference, so it must not be used
	 * here.  Capture errno before any diagnostic helper can disturb it. */
	int socket_error = result < 0 ? lwip_err : 0;
	int tcp_ok = -1;
	int ncm_ok = -1;
	int signal_pressure = 0;
	uint32_t now_us = hal_read_curtime_us();

	if (!screen_block_is_sender()) {
		return;
	}
#if defined(CONFIG_SCREEN_TCP_ACK_PROFILE) && CONFIG_SCREEN_TCP_ACK_PROFILE
	/* Register on normal traffic as well: waiting for the first EAGAIN loses
	 * the baseline window and can leave quiet 10-second windows unprofiled. */
	(void)lwip_diag_screen_tcp_ack_track(socket);
#endif
	if (result < 0) {
		tcp_ok = lwip_diag_tcp_buffer_state(socket, &tcp);
		ncm_ok = rltk_ncm_tx_diag_snapshot(&ncm);
	}

	taskENTER_CRITICAL();
	screen_block_stats.writes++;
	if (result >= 0) {
		screen_block_stats.successful++;
		if ((size_t)result < requested) {
			screen_block_stats.partial++;
		}
		if (screen_block_active) {
			uint32_t recovery_us = now_us - screen_block_start_us;

			screen_block_stats.block_recoveries++;
			screen_block_stats.block_retries +=
				screen_block_active_retries;
			screen_block_stats.recovery_sum_us += recovery_us;
			screen_block_max(&screen_block_stats.recovery_max_us,
					 recovery_us);
			screen_block_active = 0U;
			screen_block_active_retries = 0U;
			screen_block_pressure_signaled = 0U;
		}
	} else {
		screen_block_stats.last_errno = socket_error;
		if ((socket_error == EAGAIN) || (socket_error == EWOULDBLOCK)) {
			screen_block_stats.eagain++;
			if (!screen_block_active) {
				screen_block_active = 1U;
				screen_block_start_us = now_us;
				screen_block_active_retries = 1U;
				screen_block_pressure_signaled = 0U;
				screen_block_stats.block_episodes++;
			} else {
				screen_block_active_retries++;
			}
#if CONFIG_SCREEN_TX_PRESSURE_FEEDBACK && CONFIG_SCREEN_RX_RATE_LIMIT
			if (!screen_block_pressure_signaled &&
			    ((now_us - screen_block_start_us) >=
			     CONFIG_SCREEN_TX_PRESSURE_TRIGGER_MS * 1000U)) {
				screen_block_pressure_signaled = 1U;
				signal_pressure = 1;
			}
#endif
		} else {
			screen_block_stats.other_error++;
		}
		if (tcp_ok == 0) {
			uint32_t window_available =
				tcp.tx_send_window > tcp.tx_unacked_bytes ?
				tcp.tx_send_window - tcp.tx_unacked_bytes : 0U;

			screen_block_stats.tx_buffer_capacity =
				tcp.tx_buffer_capacity;
			screen_block_stats.tx_queue_capacity = tcp.tx_queue_capacity;
			screen_block_min(&screen_block_stats.tx_buffer_min,
					 tcp.tx_buffer_available);
			screen_block_max(&screen_block_stats.tx_queue_max,
					 tcp.tx_queue_len);
			screen_block_max(&screen_block_stats.unsent_bytes_max,
					 tcp.tx_unsent_bytes);
			screen_block_max(&screen_block_stats.unsent_segments_max,
					 tcp.tx_unsent_segments);
			screen_block_max(&screen_block_stats.unacked_bytes_max,
					 tcp.tx_unacked_bytes);
			screen_block_max(&screen_block_stats.unacked_segments_max,
					 tcp.tx_unacked_segments);
			screen_block_min(&screen_block_stats.send_window_min,
					 tcp.tx_send_window);
			screen_block_min(&screen_block_stats.send_window_available_min,
					 window_available);
			screen_block_min(&screen_block_stats.congestion_window_min,
					 tcp.tx_congestion_window);
			screen_block_stats.buffer_empty +=
				tcp.tx_buffer_available == 0U;
			screen_block_stats.queue_full +=
				tcp.tx_queue_len >= tcp.tx_queue_capacity;
			screen_block_stats.send_window_limited +=
				window_available < tcp.tx_mss;
		} else {
			screen_block_stats.probe_error++;
		}
		if (ncm_ok == 0) {
			screen_block_stats.ncm_queue_capacity = ncm.queue_capacity;
			screen_block_max(&screen_block_stats.ncm_queue_max,
					 ncm.queue_depth);
			screen_block_max(&screen_block_stats.ncm_inflight_max,
					 ncm.inflight_packets);
			screen_block_max(&screen_block_stats.ncm_inflight_age_max_us,
					 ncm.inflight_age_us);
		}
	}
	taskEXIT_CRITICAL();
#if CONFIG_SCREEN_TX_PRESSURE_FEEDBACK && CONFIG_SCREEN_RX_RATE_LIMIT
	if (signal_pressure) {
		lwip_screen_rx_rate_limit_pressure();
	}
#endif
	/* Preserve the exact error observed by the closed sender. */
	if (result < 0) {
		lwip_err = socket_error;
	}
#else
	(void)socket;
	(void)requested;
	(void)result;
#endif
}

void carbox_screen_block_profile_report(uint32_t sequence)
{
#if CONFIG_SCREEN_BLOCK_PROFILE || CONFIG_SCREEN_USB_PROBE
	screen_block_stats_t stats;
	uint32_t pending;
	uint32_t pending_retries;
	uint32_t pending_age_us;

	taskENTER_CRITICAL();
	stats = screen_block_stats;
	pending = screen_block_active;
	pending_retries = screen_block_active_retries;
	pending_age_us = pending ?
		hal_read_curtime_us() - screen_block_start_us : 0U;
		screen_block_stats = (screen_block_stats_t){
		.tx_buffer_min = UINT32_MAX,
		.send_window_min = UINT32_MAX,
		.send_window_available_min = UINT32_MAX,
		.congestion_window_min = UINT32_MAX,
	};
	taskEXIT_CRITICAL();
#if CONFIG_SCREEN_USB_PROBE
	{
		usb_screen_probe_stats_t usb;
		uint32_t cycles_per_us = SystemCoreClock / 1000000U;
		uint32_t usb_avg_us;
		uint32_t usb_max_us;

		usb_screen_probe_snapshot(&usb);
		if (stats.writes == 0U && usb.submits == 0U) {
			return;
		}
		usb_avg_us = usb.completions != 0U && cycles_per_us != 0U ?
			usb.completion_cycles / cycles_per_us / usb.completions : 0U;
		usb_max_us = cycles_per_us != 0U ?
			usb.completion_cycles_max / cycles_per_us : 0U;
		rt_printf("[SCREENUSB][%lu] block eagain/episodes/recover/pending="
			  "%lu/%lu/%lu/%lu tcp sndbuf_min/wnd_min="
			  "%lu/%lu limited buf0/wnd=%lu/%lu ncm qmax/cap="
			  "%lu/%lu inflight max/age_us=%lu/%lu usb submit/error/complete="
			  "%lu/%lu/%lu pending now/max=%lu/%lu complete_us avg/max="
			  "%lu/%lu\r\n",
			  (unsigned long)sequence,
			  (unsigned long)stats.eagain,
			  (unsigned long)stats.block_episodes,
			  (unsigned long)stats.block_recoveries,
			  (unsigned long)pending,
			  (unsigned long)(stats.tx_buffer_min == UINT32_MAX ? 0U :
					  stats.tx_buffer_min),
			  (unsigned long)(stats.send_window_available_min == UINT32_MAX ?
					  0U : stats.send_window_available_min),
			  (unsigned long)stats.buffer_empty,
			  (unsigned long)stats.send_window_limited,
			  (unsigned long)stats.ncm_queue_max,
			  (unsigned long)stats.ncm_queue_capacity,
			  (unsigned long)stats.ncm_inflight_max,
			  (unsigned long)stats.ncm_inflight_age_max_us,
			  (unsigned long)usb.submits,
			  (unsigned long)usb.submit_errors,
			  (unsigned long)usb.completions,
			  (unsigned long)usb.pending_now,
			  (unsigned long)usb.pending_max,
			  (unsigned long)usb_avg_us,
			  (unsigned long)usb_max_us);
	}
#else
	if (stats.writes == 0U) {
		return;
	}
	rt_printf("[SCREENBLOCK][%lu] write ok/partial/eagain/other/probe_err="
		  "%lu/%lu/%lu/%lu/%lu last_errno=%ld\r\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.successful, (unsigned long)stats.partial,
		  (unsigned long)stats.eagain, (unsigned long)stats.other_error,
		  (unsigned long)stats.probe_error, (long)stats.last_errno);
	rt_printf("[SCREENBLOCK][%lu] episode start/recover/pending="
		  "%lu/%lu/%lu retries=%lu pending_retries/age_us=%lu/%lu "
		  "recovery_us_avg/max=%llu/%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.block_episodes,
		  (unsigned long)stats.block_recoveries,
		  (unsigned long)pending,
		  (unsigned long)stats.block_retries,
		  (unsigned long)pending_retries,
		  (unsigned long)pending_age_us,
		  (unsigned long long)(stats.block_recoveries != 0U ?
			stats.recovery_sum_us / stats.block_recoveries : 0U),
		  (unsigned long)stats.recovery_max_us);
	if ((stats.eagain == 0U) && (stats.other_error == 0U)) {
		return;
	}
	rt_printf("[SCREENBLOCK][%lu] tcp buf_min/cap=%lu/%luB qlen_max/cap="
		  "%lu/%lu unsent_max=%luB/%lu unacked_max=%luB/%lu "
		  "wnd_min/avail_min/cwnd_min=%lu/%lu/%lu "
		  "limited buf0/qlen/wnd=%lu/%lu/%lu "
		  "ncm qmax/cap=%lu/%lu inflight_max/age_max_us=%lu/%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)(stats.tx_buffer_min == UINT32_MAX ? 0U :
				  stats.tx_buffer_min),
		  (unsigned long)stats.tx_buffer_capacity,
		  (unsigned long)stats.tx_queue_max,
		  (unsigned long)stats.tx_queue_capacity,
		  (unsigned long)stats.unsent_bytes_max,
		  (unsigned long)stats.unsent_segments_max,
		  (unsigned long)stats.unacked_bytes_max,
		  (unsigned long)stats.unacked_segments_max,
		  (unsigned long)(stats.send_window_min == UINT32_MAX ? 0U :
				  stats.send_window_min),
		  (unsigned long)(stats.send_window_available_min == UINT32_MAX ?
				  0U : stats.send_window_available_min),
		  (unsigned long)(stats.congestion_window_min == UINT32_MAX ? 0U :
				  stats.congestion_window_min),
		  (unsigned long)stats.buffer_empty,
		  (unsigned long)stats.queue_full,
		  (unsigned long)stats.send_window_limited,
		  (unsigned long)stats.ncm_queue_max,
		  (unsigned long)stats.ncm_queue_capacity,
		  (unsigned long)stats.ncm_inflight_max,
		  (unsigned long)stats.ncm_inflight_age_max_us);
#endif
#else
	(void)sequence;
#endif
}

#if CONFIG_SCREEN_USB_PROBE
static void carbox_screen_usb_probe_task(void *argument)
{
	uint32_t sequence = 0U;

	(void)argument;
	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(10000U));
		carbox_screen_block_profile_report(++sequence);
	}
}

void carbox_screen_usb_probe_start(void)
{
	if (xTaskCreate(carbox_screen_usb_probe_task, "screenusb",
			1024U / sizeof(StackType_t), NULL,
			tskIDLE_PRIORITY + 1U, NULL) != pdPASS) {
		rt_printf("[SCREENUSB] ERROR: reporter task creation failed\r\n");
	}
}
#else
void carbox_screen_usb_probe_start(void) { }
#endif

void carbox_screen_tx_direct_crypto_report(uint32_t sequence)
{
	screen_tx_direct_stats_t stats;
	uint32_t live = 0U;
	uint32_t i;
	uint8_t enabled;

	taskENTER_CRITICAL();
	stats = screen_tx_stats;
	screen_tx_stats = (screen_tx_direct_stats_t){ 0 };
	for (i = 0U; i < SCREEN_TX_DIRECT_SLOTS; i++) {
		live += screen_tx_slots[i].state != SCREEN_TX_DIRECT_EMPTY ? 1U : 0U;
	}
	enabled = screen_tx_enabled;
	taskEXIT_CRITICAL();

	rt_printf("[TXCRYPTO_DIRECT][%lu] enabled=%u alloc/header/attempt/defer="
		  "%lu/%lu/%lu/%lu success aes/chacha=%lu/%lu "
		  "bytes_saved=%llu\r\n", (unsigned long)sequence, enabled,
		  (unsigned long)stats.allocations,
		  (unsigned long)stats.headers,
		  (unsigned long)stats.attempts,
		  (unsigned long)stats.deferred,
		  (unsigned long)stats.aes_success,
		  (unsigned long)stats.chacha_success,
		  (unsigned long long)stats.bytes_saved);
	rt_printf("[TXCRYPTO_DIRECT][%lu] fallback/failure/stale_write="
		  "%lu/%lu/%lu prefix same/rewritten=%lu/%lu "
		  "release/table_full/live=%lu/%lu/%lu\r\n",
		  (unsigned long)sequence, (unsigned long)stats.fallbacks,
		  (unsigned long)stats.failures,
		  (unsigned long)stats.stale_write,
		  (unsigned long)stats.prefix_same,
		  (unsigned long)stats.prefix_rewritten,
		  (unsigned long)stats.releases,
		  (unsigned long)stats.table_full, (unsigned long)live);
	rt_printf("[TXCRYPTO_DIRECT][%lu] tcp_owned begin/defer/complete/final="
		  "%lu/%lu/%lu/%lu\r\n", (unsigned long)sequence,
		  (unsigned long)stats.owned_begin,
		  (unsigned long)stats.owned_deferred_free,
		  (unsigned long)stats.owned_completions,
		  (unsigned long)stats.owned_final_frees);
}

void carbox_screen_tx_pacer_report(uint32_t sequence)
{
#if CONFIG_SCREEN_TX_PACER
	screen_tx_pacer_stats_t stats;
	uint32_t tokens;

	taskENTER_CRITICAL();
	stats = screen_tx_pacer_stats;
	screen_tx_pacer_stats = (screen_tx_pacer_stats_t){ 0 };
	tokens = screen_tx_pacer_tokens;
	taskEXIT_CRITICAL();
	if (stats.calls == 0U) {
		return;
	}
	rt_printf("[SCREENTXPACER][%lu] target=%lu kbps "
		  "requested/allowed/written=%llu/%llu/%lluB "
		  "calls/partial/wait=%lu/%lu/%lu wait_us avg/max=%llu/%lu "
		  "tokens/cap=%lu/%luB limits chunk=%luB wait_ms=%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)(CONFIG_SCREEN_TX_PACER_BPS / 1000U),
		  (unsigned long long)stats.requested_bytes,
		  (unsigned long long)stats.allowed_bytes,
		  (unsigned long long)stats.written_bytes,
		  (unsigned long)stats.calls,
		  (unsigned long)stats.partials,
		  (unsigned long)stats.waits,
		  (unsigned long long)(stats.waits != 0U ?
			stats.wait_sum_us / stats.waits : 0U),
		  (unsigned long)stats.wait_max_us,
		  (unsigned long)tokens,
		  (unsigned long)CONFIG_SCREEN_TX_PACER_BUCKET_BYTES,
		  (unsigned long)CONFIG_SCREEN_TX_PACER_CHUNK_BYTES,
		  (unsigned long)CONFIG_SCREEN_TX_PACER_WAIT_MS);
#else
	(void)sequence;
#endif
}

#if !CONFIG_SCREEN_QUEUE_PROFILE
/* The full queue profiler normally owns this linker wrapper. Keep the
 * pre-write transaction validation present in production builds where that
 * profiler is disabled; the actual socket behavior remains untouched. */
extern int __real_lwip_write(int socket, const void *buffer, size_t bytes);

int __wrap_lwip_write(int socket, const void *buffer, size_t bytes)
{
	int result;
	size_t paced_bytes = bytes;

	carbox_screen_tx_before_write(buffer, bytes);
#if CONFIG_SCREEN_TX_PACER
	paced_bytes = carbox_screen_tx_pacer_allowance(bytes);
#endif
#if CONFIG_TCP_OWNED_WRITE
	if (carbox_screen_tx_owned_begin(buffer, paced_bytes)) {
		result = lwip_write_owned(socket, buffer, paced_bytes,
			carbox_screen_tx_owned_complete, (void *)(uintptr_t)buffer);
	} else {
		result = __real_lwip_write(socket, buffer, paced_bytes);
	}
#else
	result = __real_lwip_write(socket, buffer, paced_bytes);
#endif
#if CONFIG_SCREEN_TX_PACER
	carbox_screen_tx_pacer_complete(paced_bytes, result);
#endif
	carbox_screen_block_profile_write(socket, bytes, result);
	return result;
}
#endif

#else

void carbox_screen_tx_allocation(void *pointer, size_t length)
{
	(void)pointer;
	(void)length;
}
void carbox_screen_tx_release(void *pointer) { (void)pointer; }
int carbox_screen_tx_owned_begin(const void *pointer, size_t length)
{
	(void)pointer;
	(void)length;
	return 0;
}
int carbox_screen_tx_owned_consumer_release(void *pointer)
{
	(void)pointer;
	return 0;
}
void carbox_screen_tx_owned_complete(void *pointer) { (void)pointer; }
void *carbox_airplay_screen_memcpy(void *destination, const void *source,
				   size_t length)
{
	return memcpy(destination, source, length);
}
int carbox_screen_tx_crypto_begin(void *alias_source, size_t length,
				  void *destination, uint32_t kind,
				  const void **direct_source)
{
	(void)length;
	(void)destination;
	(void)kind;
	if (direct_source != NULL) *direct_source = alias_source;
	return 0;
}
int carbox_screen_tx_crypto_active(void *destination, size_t length,
				   const void **direct_source)
{
	(void)length;
	if (direct_source != NULL) *direct_source = destination;
	return 0;
}
void carbox_screen_tx_crypto_materialized(void *destination, size_t length)
{
	(void)destination;
	(void)length;
}
void carbox_screen_tx_crypto_complete(void *destination, size_t length,
				      uint32_t kind, int status)
{
	(void)destination;
	(void)length;
	(void)kind;
	(void)status;
}
void carbox_screen_tx_before_write(const void *buffer, size_t length)
{
	(void)buffer;
	(void)length;
}
void carbox_screen_block_profile_write(int socket, size_t requested, int result)
{
	(void)socket;
	(void)requested;
	(void)result;
}
size_t carbox_screen_tx_pacer_allowance(size_t requested)
{
	return requested;
}
void carbox_screen_tx_pacer_complete(size_t allowed, int result)
{
	(void)allowed;
	(void)result;
}
void carbox_screen_tx_direct_crypto_report(uint32_t sequence)
{
	(void)sequence;
}
void carbox_screen_tx_pacer_report(uint32_t sequence)
{
	(void)sequence;
}
void carbox_screen_block_profile_report(uint32_t sequence)
{
	(void)sequence;
}
void carbox_screen_usb_probe_start(void) { }

#endif
