/* Observe each vendor RX frame pool without printing in the RX critical section. */
#include <stdint.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#ifndef CONFIG_RX_HANDOVER_DIAG
#define CONFIG_RX_HANDOVER_DIAG 0
#endif

extern void printf_lock_init(void);

#if CONFIG_RX_HANDOVER_DIAG
#define WLAN_RECVFRAME_POOL_SIZE 32U
#define WLAN_RECVFRAME_SLOTS 4U
#define WLAN_RECVFRAME_REPORT_MS 2000U

extern int __real_rtw_queue_empty(void *queue);
extern void *_rtw_alloc_recvframe(void *queue);

typedef struct {
	void *queue;
	uint32_t checks;
	uint32_t empty;
	uint32_t total_empty;
	uint32_t recovered;
	uint32_t empty_run;
	uint32_t empty_run_max;
	uint32_t free_min;
	uint32_t free_last;
	uint32_t invalid_count;
	uint32_t rx_drop;
	uint32_t skb_drop;
	uint32_t counter_resets;
	uint16_t rx_counter;
	uint16_t skb_counter;
	uint8_t counter_valid;
} recvframe_stats_t;

static volatile recvframe_stats_t recvframe_stats[WLAN_RECVFRAME_SLOTS];
static volatile uint32_t untracked_checks;

static uint32_t counter_delta(uint16_t current, uint16_t previous,
			      volatile uint32_t *resets)
{
	if (current >= previous)
		return (uint32_t)(current - previous);
	if (previous >= 65000U && current < 1000U)
		return (uint32_t)(uint16_t)(current - previous);
	(*resets)++;
	return 0U;
}

int __wrap_rtw_queue_empty(void *queue)
{
	const int empty = __real_rtw_queue_empty(queue);
	const uintptr_t caller = (uintptr_t)__builtin_return_address(0) & ~(uintptr_t)1;
	const uintptr_t alloc = (uintptr_t)_rtw_alloc_recvframe & ~(uintptr_t)1;
	volatile recvframe_stats_t *stats = NULL;
	uint32_t i;
	uint32_t free_count;
	uint16_t rx_counter, skb_counter;

	/* Verified vendor object: this call returns at _rtw_alloc_recvframe+0x08.
	 * The queue is at adapter+0xc44; free count is queue+0x2c. The recovered
	 * tasklet increments adapter+0xca0 when it drops a packet for lack of a
	 * recvframe, or adapter+0xca2 when its skb allocation fails. The latter
	 * counters are sampled on the next allocation attempt, so their report
	 * may lag the actual failure by one RX packet.
	 */
	if (caller != alloc + 0x08U)
		return empty;
	for (i = 0U; i < WLAN_RECVFRAME_SLOTS; ++i) {
		if (recvframe_stats[i].queue == queue) {
			stats = &recvframe_stats[i];
			break;
		}
		if (stats == NULL && recvframe_stats[i].queue == NULL)
			stats = &recvframe_stats[i];
	}
	if (i == WLAN_RECVFRAME_SLOTS &&
	    (stats == NULL || stats->queue != NULL)) {
		untracked_checks++;
		return empty;
	}
	if (stats->queue == NULL) {
		stats->queue = queue;
		stats->free_min = WLAN_RECVFRAME_POOL_SIZE;
	}

	free_count = *(volatile uint32_t *)((uintptr_t)queue + 0x2cU);
	rx_counter = *(volatile uint16_t *)((uintptr_t)queue + 0x5cU);
	skb_counter = *(volatile uint16_t *)((uintptr_t)queue + 0x5eU);
	stats->checks++;
	if (free_count <= WLAN_RECVFRAME_POOL_SIZE) {
		stats->free_last = free_count;
		if (free_count < stats->free_min)
			stats->free_min = free_count;
	} else {
		stats->invalid_count++;
	}
	if (empty == 1) {
		stats->empty++;
		stats->total_empty++;
		stats->empty_run++;
		if (stats->empty_run > stats->empty_run_max)
			stats->empty_run_max = stats->empty_run;
	} else if (stats->empty_run != 0U) {
		stats->recovered++;
		stats->empty_run = 0U;
	}
	if (stats->counter_valid) {
		stats->rx_drop += counter_delta(rx_counter, stats->rx_counter,
						 &stats->counter_resets);
		stats->skb_drop += counter_delta(skb_counter, stats->skb_counter,
						  &stats->counter_resets);
	}
	stats->rx_counter = rx_counter;
	stats->skb_counter = skb_counter;
	stats->counter_valid = 1U;
	return empty;
}

#endif /* CONFIG_RX_HANDOVER_DIAG */

static void wlan_recvframe_diag_task(void *argument)
{
#if CONFIG_RX_HANDOVER_DIAG
	uint32_t quiet_windows[WLAN_RECVFRAME_SLOTS] = { 0U };
	uint8_t report_after_event[WLAN_RECVFRAME_SLOTS] = { 0U };
#endif
	(void)argument;
	printf_lock_init();

#if CONFIG_RX_HANDOVER_DIAG
	for (;;) {
		uint32_t i;
		vTaskDelay(pdMS_TO_TICKS(WLAN_RECVFRAME_REPORT_MS));
		for (i = 0U; i < WLAN_RECVFRAME_SLOTS; ++i) {
			volatile recvframe_stats_t *stats = &recvframe_stats[i];
			void *queue;
			uint32_t checks, empty, total_empty, recovered;
			uint32_t empty_run_max, free_min, free_last, invalid_count;
			uint32_t rx_drop, skb_drop, counter_resets;
			uint8_t event;

			taskENTER_CRITICAL();
			queue = stats->queue;
			checks = stats->checks;
			empty = stats->empty;
			total_empty = stats->total_empty;
			recovered = stats->recovered;
			empty_run_max = stats->empty_run_max;
			free_min = stats->free_min;
			free_last = stats->free_last;
			invalid_count = stats->invalid_count;
			rx_drop = stats->rx_drop;
			skb_drop = stats->skb_drop;
			counter_resets = stats->counter_resets;
			stats->checks = 0U;
			stats->empty = 0U;
			stats->recovered = 0U;
			stats->empty_run_max = 0U;
			stats->invalid_count = 0U;
			stats->rx_drop = 0U;
			stats->skb_drop = 0U;
			stats->counter_resets = 0U;
			stats->free_min = WLAN_RECVFRAME_POOL_SIZE;
			taskEXIT_CRITICAL();

			if (checks == 0U)
				continue;
			event = (empty != 0U || free_min <= 4U || rx_drop != 0U ||
				 skb_drop != 0U || invalid_count != 0U);
			if (event || report_after_event[i] || ++quiet_windows[i] >= 5U) {
				printf("[WLAN_RECVFRAME] tick=%lu queue=%p 2s checks=%lu free_min=%lu free_last=%lu empty=%lu total=%lu empty_run_max=%lu recovered=%lu rx_drop=%lu skb_drop=%lu invalid=%lu counter_resets=%lu pool=32\n",
				       (unsigned long)xTaskGetTickCount(), queue,
				       (unsigned long)checks, (unsigned long)free_min,
				       (unsigned long)free_last, (unsigned long)empty,
				       (unsigned long)total_empty,
				       (unsigned long)empty_run_max,
				       (unsigned long)recovered,
				       (unsigned long)rx_drop, (unsigned long)skb_drop,
				       (unsigned long)invalid_count,
				       (unsigned long)counter_resets);
				quiet_windows[i] = 0U;
			}
			report_after_event[i] = event;
		}
		if (untracked_checks != 0U) {
			uint32_t count;
			taskENTER_CRITICAL();
			count = untracked_checks;
			untracked_checks = 0U;
			taskEXIT_CRITICAL();
			printf("[WLAN_RECVFRAME] untracked_queue_checks=%lu\n",
			       (unsigned long)count);
		}
	}
#else
	/* Keep UART serialization, then release this startup task and its stack. */
	vTaskDelete(NULL);
#endif
}

void carbox_wlan_recvframe_diag_start(void)
{
	if (xTaskCreate(wlan_recvframe_diag_task, "wlan_rx_diag", 1024U, NULL,
			tskIDLE_PRIORITY + 1U, NULL) != pdPASS)
		printf("[WLAN_RECVFRAME] diagnostic task create failed\n");
}
