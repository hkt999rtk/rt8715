#include "crypto_priority_lock.h"

#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"
#include "osdep_service.h"

#ifndef CARBOX_CRYPTO_OWNER_BOOST_PRIORITY
#define CARBOX_CRYPTO_OWNER_BOOST_PRIORITY 11
#endif

#if CARBOX_CRYPTO_OWNER_BOOST_PRIORITY >= configMAX_PRIORITIES
#error "CARBOX_CRYPTO_OWNER_BOOST_PRIORITY must be below configMAX_PRIORITIES"
#endif

#define CARBOX_CRYPTO_PRIORITY_SLOTS 8u

typedef struct {
	TaskHandle_t task;
	UBaseType_t original_priority;
	unsigned kind;
	unsigned depth;
} carbox_crypto_priority_slot_t;

static carbox_crypto_priority_slot_t crypto_priority_slots[
	CARBOX_CRYPTO_PRIORITY_SLOTS
];
static unsigned crypto_priority_slot_exhausted_reported;

static carbox_crypto_priority_slot_t *crypto_priority_find_slot(
	TaskHandle_t task, int allocate
)
{
	carbox_crypto_priority_slot_t *free_slot = NULL;
	unsigned i;

	for (i = 0; i < CARBOX_CRYPTO_PRIORITY_SLOTS; ++i) {
		carbox_crypto_priority_slot_t *slot = &crypto_priority_slots[i];

		if (slot->task == task) return slot;
		if (!slot->task && !free_slot) free_slot = slot;
	}
	if (allocate && free_slot) free_slot->task = task;
	return allocate ? free_slot : NULL;
}

static void crypto_priority_enter(unsigned kind)
{
	TaskHandle_t task;
	carbox_crypto_priority_slot_t *slot;
	UBaseType_t priority;
	int report_exhaustion = 0;

	if (rtw_in_interrupt()) return;
	task = xTaskGetCurrentTaskHandle();
	if (!task) return;
	priority = uxTaskPriorityGet(task);

	taskENTER_CRITICAL();
	slot = crypto_priority_find_slot(task, 1);
	if (slot) {
		if (slot->depth++ == 0u) {
			slot->original_priority = priority;
			slot->kind = kind;
		}
	} else if (!crypto_priority_slot_exhausted_reported) {
		crypto_priority_slot_exhausted_reported = 1u;
		report_exhaustion = 1;
	}
	taskEXIT_CRITICAL();
	if (report_exhaustion) {
		printf("[CRYPTO][PRIO] slot table exhausted; boost skipped\n");
	}

	if (slot && (CARBOX_CRYPTO_OWNER_BOOST_PRIORITY > 0) &&
	    priority < CARBOX_CRYPTO_OWNER_BOOST_PRIORITY) {
		vTaskPrioritySet(task, CARBOX_CRYPTO_OWNER_BOOST_PRIORITY);
	}
}

static void crypto_priority_leave(void)
{
	TaskHandle_t task;
	carbox_crypto_priority_slot_t *slot;
	UBaseType_t original_priority = 0;
	int restore = 0;

	if (rtw_in_interrupt()) return;
	task = xTaskGetCurrentTaskHandle();
	if (!task) return;

	taskENTER_CRITICAL();
	slot = crypto_priority_find_slot(task, 0);
	if (slot && slot->depth) {
		if (--slot->depth == 0u) {
			original_priority = slot->original_priority;
			slot->task = NULL;
			restore = 1;
		}
	}
	taskEXIT_CRITICAL();

	if (restore && uxTaskPriorityGet(task) != original_priority) {
		vTaskPrioritySet(task, original_priority);
	}
}

static void crypto_priority_device_lock(RT_DEV_LOCK_E device, unsigned kind)
{
	if (device == RT_DEV_LOCK_CRYPTO) crypto_priority_enter(kind);
	device_mutex_lock(device);
}

static void crypto_priority_device_unlock(RT_DEV_LOCK_E device)
{
	/*
	 * Release first while still at the boosted priority.  Priority 11 is the
	 * platform maximum, so the current task can restore its slot immediately
	 * without leaving a lower-priority task holding the engine lock.
	 */
	device_mutex_unlock(device);
	if (device == RT_DEV_LOCK_CRYPTO) crypto_priority_leave();
}

/* Separate public entries preserve AES/ChaCha attribution in the profiler. */
void carbox_crypto_aes_device_lock(RT_DEV_LOCK_E device)
{
	crypto_priority_device_lock(device, CARBOX_CRYPTO_KIND_AES);
}

void carbox_crypto_aes_device_unlock(RT_DEV_LOCK_E device)
{
	crypto_priority_device_unlock(device);
}

void carbox_crypto_chacha_device_lock(RT_DEV_LOCK_E device)
{
	crypto_priority_device_lock(device, CARBOX_CRYPTO_KIND_CHACHA);
}

void carbox_crypto_chacha_device_unlock(RT_DEV_LOCK_E device)
{
	crypto_priority_device_unlock(device);
}

unsigned carbox_crypto_priority_current_kind(void)
{
	TaskHandle_t task;
	carbox_crypto_priority_slot_t *slot;
	unsigned kind = CARBOX_CRYPTO_KIND_NONE;

	if (rtw_in_interrupt()) return kind;
	task = xTaskGetCurrentTaskHandle();
	if (!task) return kind;
	taskENTER_CRITICAL();
	slot = crypto_priority_find_slot(task, 0);
	if (slot && slot->depth) kind = slot->kind;
	taskEXIT_CRITICAL();
	return kind;
}
