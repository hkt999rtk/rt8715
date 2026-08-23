#include <stdint.h>

#ifndef CONFIG_IAP2_DEVICE_TIME_SYNC
#define CONFIG_IAP2_DEVICE_TIME_SYNC 0
#endif

#if CONFIG_IAP2_DEVICE_TIME_SYNC

#include <string.h>
#include <time.h>

#include "FreeRTOS.h"
#include "task.h"
#include "diag.h"

#define IAP2_TIME_MIN_YEAR 2020
#define IAP2_TIME_MAX_YEAR 2037 /* sntp_set_lasttime() stores signed long */

static uint8_t iap2_device_time_sync_done;

extern void __real_UpdateDeviceTime(int year, int month, int day, int hour,
				    int minute, int second);
extern void sntp_set_lasttime(long sec, long usec, unsigned int tick);

static uint8_t iap2_device_time_fields_valid(int year, int month, int day,
					      int hour, int minute, int second)
{
	return year >= IAP2_TIME_MIN_YEAR && year <= IAP2_TIME_MAX_YEAR &&
	       month >= 1 && month <= 12 && day >= 1 && day <= 31 &&
	       hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59 &&
	       second >= 0 && second <= 60;
}

static void iap2_try_sync_device_time(int year, int month, int day, int hour,
				      int minute, int second)
{
	struct tm local_time;
	time_t epoch;
	uint8_t claim_sync = 0U;

	if (iap2_device_time_sync_done ||
	    !iap2_device_time_fields_valid(year, month, day, hour, minute, second)) {
		return;
	}

	memset(&local_time, 0, sizeof(local_time));
	local_time.tm_year = year - 1900;
	local_time.tm_mon = month - 1;
	local_time.tm_mday = day;
	local_time.tm_hour = hour;
	local_time.tm_min = minute;
	local_time.tm_sec = second;
	local_time.tm_isdst = -1;
	epoch = mktime(&local_time);
	if (epoch == (time_t)-1) {
		return;
	}

	taskENTER_CRITICAL();
	if (!iap2_device_time_sync_done) {
		sntp_set_lasttime((long)epoch, 0L, xTaskGetTickCount());
		iap2_device_time_sync_done = 1U;
		claim_sync = 1U;
	}
	taskEXIT_CRITICAL();

	if (claim_sync) {
		rt_printf("[IAP2TIME] local clock synchronized "
			  "%04d-%02d-%02d %02d:%02d:%02d epoch=%ld\n",
			  year, month, day, hour, minute, second, (long)epoch);
	}
}

void __wrap_UpdateDeviceTime(int year, int month, int day, int hour,
			     int minute, int second)
{
	iap2_try_sync_device_time(year, month, day, hour, minute, second);
	__real_UpdateDeviceTime(year, month, day, hour, minute, second);
}

#endif
