#include "screen_tx_direct_crypto.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "diag.h"

#ifndef CONFIG_SCREEN_TX_DIRECT_CRYPTO
#define CONFIG_SCREEN_TX_DIRECT_CRYPTO 0
#endif

#ifndef SCREEN_TX_DIRECT_CRYPTO_MIN_BYTES
#define SCREEN_TX_DIRECT_CRYPTO_MIN_BYTES 4096U
#endif

#if CONFIG_SCREEN_TX_DIRECT_CRYPTO

#define SCREEN_TX_DIRECT_SLOTS 4U
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
	uint64_t bytes_saved;
} screen_tx_direct_stats_t;

static screen_tx_direct_slot_t screen_tx_slots[SCREEN_TX_DIRECT_SLOTS];
static screen_tx_direct_stats_t screen_tx_stats;
static uint8_t screen_tx_enabled = 1U;

static screen_tx_direct_slot_t *screen_tx_find_task_locked(TaskHandle_t task)
{
	uint32_t i;

	for (i = 0U; i < SCREEN_TX_DIRECT_SLOTS; i++) {
		if (screen_tx_slots[i].owner == task) {
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
}

/* AESUtils.o is preserved from the vendor archive. Linker wrapping changes
 * only the API arguments for the exact deferred ScreenThread transaction. */
extern int __real_AES_CTR_Update(void *context, const void *source,
				 size_t length, void *destination);

int __wrap_AES_CTR_Update(void *context, const void *source, size_t length,
			 void *destination)
{
	const void *direct_source = source;
	int direct = carbox_screen_tx_crypto_begin((void *)(uintptr_t)source,
		length, destination, CARBOX_SCREEN_TX_CRYPTO_AES,
		&direct_source);
	int status = __real_AES_CTR_Update(context, direct_source, length,
		destination);

	if (direct) {
		carbox_screen_tx_crypto_complete(destination, length,
			CARBOX_SCREEN_TX_CRYPTO_AES, status);
	}
	return status;
}

#if !CONFIG_SCREEN_QUEUE_PROFILE
/* The full queue profiler normally owns this linker wrapper. Keep the
 * pre-write transaction validation present in production builds where that
 * profiler is disabled; the actual socket behavior remains untouched. */
extern int __real_lwip_write(int socket, const void *buffer, size_t bytes);

int __wrap_lwip_write(int socket, const void *buffer, size_t bytes)
{
	carbox_screen_tx_before_write(buffer, bytes);
	return __real_lwip_write(socket, buffer, bytes);
}
#endif

#else

void carbox_screen_tx_allocation(void *pointer, size_t length)
{
	(void)pointer;
	(void)length;
}
void carbox_screen_tx_release(void *pointer) { (void)pointer; }
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
void carbox_screen_tx_direct_crypto_report(uint32_t sequence)
{
	(void)sequence;
}

#endif
