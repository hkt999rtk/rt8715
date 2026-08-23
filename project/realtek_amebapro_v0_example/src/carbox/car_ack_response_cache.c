#include "car_ack_response_cache.h"

#ifndef CONFIG_CAR_ACK_RESPONSE_CACHE
#define CONFIG_CAR_ACK_RESPONSE_CACHE 0
#endif

#if CONFIG_CAR_ACK_RESPONSE_CACHE

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "FreeRTOS.h"
#include "task.h"
#include "diag.h"
#include "hal_timer.h"

#define CAR_ACK_HEADER_BUFFER_OFFSET 0x010U
#define CAR_ACK_HEADER_LENGTH_OFFSET 0x410U
#define CAR_ACK_STATUS_OFFSET        0x478U
#define CAR_ACK_BODY_POINTER_OFFSET  0x49cU
#define CAR_ACK_BODY_LENGTH_OFFSET   0x4a0U
#define CAR_ACK_HEADER_CAPACITY      1024U
#define CAR_ACK_CACHE_SLOTS          2U
#define CAR_ACK_HEADER_MAX           96U

typedef struct car_ack_calendar_s {
	uint16_t year;
	uint16_t month;
	uint16_t weekday;
	uint16_t day;
	uint16_t hour;
	uint16_t minute;
	uint16_t second;
} car_ack_calendar_t;

typedef struct car_ack_cache_slot_s {
	time_t epoch;
	car_ack_calendar_t calendar;
	uint16_t length;
	uint8_t valid;
	char header[CAR_ACK_HEADER_MAX];
} car_ack_cache_slot_t;

typedef struct car_ack_cache_stats_s {
	uint32_t responses;
	uint32_t prepared_hits;
	uint32_t prepared_rewrites;
	uint32_t hits;
	uint32_t misses;
	uint32_t current_builds;
	uint32_t next_builds;
	uint32_t create_errors;
	uint32_t send_errors;
	uint64_t apply_sum_us;
	uint32_t apply_max_us;
	uint64_t build_sum_us;
	uint32_t build_max_us;
} car_ack_cache_stats_t;

static car_ack_cache_slot_t car_ack_slots[CAR_ACK_CACHE_SLOTS]
	__attribute__((section(".lpddr.bss.car_ack_slots")));
static car_ack_cache_stats_t car_ack_stats
	__attribute__((section(".lpddr.bss.car_ack_stats")));
static uint8_t car_ack_replace_slot;
static void *car_ack_prepared_message;
static time_t car_ack_prepared_epoch;

extern int32_t HTTPMessageCreate(void **message_out);
extern int32_t AirPlayEvent_SendMessage(void *message);
extern void AirPlayEvent_SendResponse(const void *body, uint32_t body_length);
extern void CFRelease(void *object);
extern void GetLocalTime(car_ack_calendar_t *calendar);

static int car_ack_is_leap_year(uint16_t year)
{
	return ((year % 4U) == 0U) &&
		(((year % 100U) != 0U) || ((year % 400U) == 0U));
}

static uint16_t car_ack_days_in_month(uint16_t year, uint16_t month)
{
	static const uint8_t days[12] = {
		31U, 28U, 31U, 30U, 31U, 30U,
		31U, 31U, 30U, 31U, 30U, 31U
	};

	if ((month == 0U) || (month > 12U)) {
		return 31U;
	}
	if ((month == 2U) && car_ack_is_leap_year(year)) {
		return 29U;
	}
	return days[month - 1U];
}

static void car_ack_calendar_add_second(car_ack_calendar_t *calendar)
{
	calendar->second++;
	if (calendar->second < 60U) return;
	calendar->second = 0U;
	calendar->minute++;
	if (calendar->minute < 60U) return;
	calendar->minute = 0U;
	calendar->hour++;
	if (calendar->hour < 24U) return;
	calendar->hour = 0U;
	calendar->weekday = (uint16_t)((calendar->weekday + 1U) % 7U);
	calendar->day++;
	if (calendar->day <= car_ack_days_in_month(calendar->year,
						      calendar->month)) return;
	calendar->day = 1U;
	calendar->month++;
	if (calendar->month <= 12U) return;
	calendar->month = 1U;
	calendar->year++;
}

static int car_ack_format_slot(car_ack_cache_slot_t *slot, time_t epoch,
			       const car_ack_calendar_t *calendar)
{
	static const char *const weekdays[7] = {
		"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
	};
	static const char *const months[12] = {
		"Jan", "Feb", "Mar", "Apr", "May", "Jun",
		"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
	};
	uint32_t start_us = hal_read_curtime_us();
	int length;

	if ((calendar->weekday >= 7U) || (calendar->month == 0U) ||
	    (calendar->month > 12U)) {
		return -1;
	}
	length = snprintf(slot->header, sizeof(slot->header),
		"RTSP/1.0 200 OK\r\n"
		"Date: %s, %02u %s %04u %02u:%02u:%02u GMT\r\n",
		weekdays[calendar->weekday], (unsigned)calendar->day,
		months[calendar->month - 1U], (unsigned)calendar->year,
		(unsigned)calendar->hour, (unsigned)calendar->minute,
		(unsigned)calendar->second);
	if ((length <= 0) || ((uint32_t)length >= sizeof(slot->header))) {
		return -1;
	}
	slot->epoch = epoch;
	slot->calendar = *calendar;
	slot->length = (uint16_t)length;
	slot->valid = 1U;
	{
		uint32_t elapsed_us = hal_read_curtime_us() - start_us;
		car_ack_stats.build_sum_us += elapsed_us;
		if (elapsed_us > car_ack_stats.build_max_us) {
			car_ack_stats.build_max_us = elapsed_us;
		}
	}
	return 0;
}

static car_ack_cache_slot_t *car_ack_find_slot(time_t epoch)
{
	uint32_t i;

	for (i = 0U; i < CAR_ACK_CACHE_SLOTS; i++) {
		if (car_ack_slots[i].valid && (car_ack_slots[i].epoch == epoch)) {
			return &car_ack_slots[i];
		}
	}
	return NULL;
}

static car_ack_cache_slot_t *car_ack_build_current(void)
{
	car_ack_cache_slot_t *slot = &car_ack_slots[car_ack_replace_slot];
	car_ack_calendar_t calendar;
	time_t before;
	time_t after;
	uint32_t attempts = 0U;

	/* Do not bind a calendar sampled just across a second boundary to the
	 * preceding epoch key.  This loop normally executes exactly once. */
	do {
		before = time(NULL);
		GetLocalTime(&calendar);
		after = time(NULL);
		attempts++;
	} while ((before != after) && (attempts < 3U));
	if ((before != after) ||
	    (car_ack_format_slot(slot, after, &calendar) != 0)) {
		return NULL;
	}
	car_ack_replace_slot ^= 1U;
	car_ack_stats.current_builds++;
	return slot;
}

static void car_ack_prepare_next(const car_ack_cache_slot_t *current)
{
	car_ack_cache_slot_t *slot;
	car_ack_calendar_t next_calendar;
	time_t next_epoch;

	if (current == NULL) return;
	next_epoch = current->epoch + 1;
	if (car_ack_find_slot(next_epoch) != NULL) return;
	next_calendar = current->calendar;
	car_ack_calendar_add_second(&next_calendar);
	slot = &car_ack_slots[car_ack_replace_slot];
	if (slot == current) {
		slot = &car_ack_slots[car_ack_replace_slot ^ 1U];
	}
	if (car_ack_format_slot(slot, next_epoch, &next_calendar) == 0) {
		car_ack_replace_slot = (uint8_t)((slot - car_ack_slots) ^ 1U);
		car_ack_stats.next_builds++;
	}
}

static void car_ack_apply_header(void *message,
				 const car_ack_cache_slot_t *slot)
{
	uint8_t *message_bytes = (uint8_t *)message;

	memcpy(message_bytes + CAR_ACK_HEADER_BUFFER_OFFSET,
	       slot->header, slot->length);
	*(uint32_t *)(message_bytes + CAR_ACK_HEADER_LENGTH_OFFSET) = slot->length;
	*(int32_t *)(message_bytes + CAR_ACK_STATUS_OFFSET) = 200;
	*(void **)(message_bytes + CAR_ACK_BODY_POINTER_OFFSET) = NULL;
	*(uint32_t *)(message_bytes + CAR_ACK_BODY_LENGTH_OFFSET) = 0U;
}

static void car_ack_replenish_message(const car_ack_cache_slot_t *last_slot)
{
	car_ack_cache_slot_t *slot;
	void *message = NULL;
	time_t now;

	if (car_ack_prepared_message != NULL) return;
	if (HTTPMessageCreate(&message) != 0 || message == NULL) {
		car_ack_stats.create_errors++;
		return;
	}
	now = time(NULL);
	slot = car_ack_find_slot(now);
	if (slot == NULL && last_slot != NULL && last_slot->epoch == now) {
		slot = (car_ack_cache_slot_t *)last_slot;
	}
	if (slot != NULL) {
		car_ack_apply_header(message, slot);
		car_ack_prepared_epoch = slot->epoch;
	} else {
		car_ack_prepared_epoch = (time_t)-1;
	}
	car_ack_prepared_message = message;
}

void carbox_airplay_event_send_fast_response(const void *body,
					      uint32_t body_length)
{
	car_ack_cache_slot_t *slot;
	void *message = NULL;
	time_t now;
	uint32_t start_us;
	int32_t result;

	(void)body;
	(void)body_length;
	car_ack_stats.responses++;
	start_us = hal_read_curtime_us();
	now = time(NULL);
	slot = car_ack_find_slot(now);
	if (slot != NULL) {
		car_ack_stats.hits++;
	} else {
		car_ack_stats.misses++;
		slot = car_ack_build_current();
	}
	if ((slot == NULL) || (slot->length >= CAR_ACK_HEADER_CAPACITY)) {
		car_ack_stats.create_errors++;
		AirPlayEvent_SendResponse(body, body_length);
		return;
	}

	message = car_ack_prepared_message;
	car_ack_prepared_message = NULL;
	if (message == NULL) {
		result = HTTPMessageCreate(&message);
		if ((result != 0) || (message == NULL)) {
			car_ack_stats.create_errors++;
			AirPlayEvent_SendResponse(body, body_length);
			return;
		}
		car_ack_apply_header(message, slot);
		car_ack_stats.prepared_rewrites++;
	} else if (car_ack_prepared_epoch == slot->epoch) {
		car_ack_stats.prepared_hits++;
	} else {
		car_ack_apply_header(message, slot);
		car_ack_stats.prepared_rewrites++;
	}
	{
		uint32_t elapsed_us = hal_read_curtime_us() - start_us;
		car_ack_stats.apply_sum_us += elapsed_us;
		if (elapsed_us > car_ack_stats.apply_max_us) {
			car_ack_stats.apply_max_us = elapsed_us;
		}
	}

	result = AirPlayEvent_SendMessage(message);
	if (result != 0) {
		car_ack_stats.send_errors++;
	} else {
		/* The current response is already accepted by the original transport.
		 * Spend the formatting work now, outside the response critical path. */
		car_ack_prepare_next(slot);
	}
	CFRelease(message);
	/* Keep allocation and complete header preparation after the previous
	 * response has been accepted by the original encrypted transport. */
	car_ack_replenish_message(slot);
}

void carbox_car_ack_response_cache_report(uint32_t sequence)
{
	car_ack_cache_stats_t stats;

	taskENTER_CRITICAL();
	stats = car_ack_stats;
	memset(&car_ack_stats, 0, sizeof(car_ack_stats));
	taskEXIT_CRITICAL();
	rt_printf("[CARACKCACHE][%lu] response/prepared/rewrite="
		  "%lu/%lu/%lu date hit/miss/build_cur/build_next="
		  "%lu/%lu/%lu/%lu error create/send=%lu/%lu "
		  "apply_us avg/max=%lu/%lu build_us avg/max=%lu/%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.responses,
		  (unsigned long)stats.prepared_hits,
		  (unsigned long)stats.prepared_rewrites,
		  (unsigned long)stats.hits,
		  (unsigned long)stats.misses,
		  (unsigned long)stats.current_builds,
		  (unsigned long)stats.next_builds,
		  (unsigned long)stats.create_errors,
		  (unsigned long)stats.send_errors,
		  stats.responses != 0U ?
			(unsigned long)(stats.apply_sum_us / stats.responses) : 0UL,
		  (unsigned long)stats.apply_max_us,
		  (stats.current_builds + stats.next_builds) != 0U ?
			(unsigned long)(stats.build_sum_us /
				(stats.current_builds + stats.next_builds)) : 0UL,
		  (unsigned long)stats.build_max_us);
}

#else

void carbox_car_ack_response_cache_report(uint32_t sequence)
{
	(void)sequence;
}

#endif
