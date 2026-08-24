#include "iap2_cond_timedwait_fix.h"

#ifndef CONFIG_IAP2_COND_TIMEDWAIT_FIX
#define CONFIG_IAP2_COND_TIMEDWAIT_FIX 0
#endif

#if CONFIG_IAP2_COND_TIMEDWAIT_FIX

#include <errno.h>

#include "FreeRTOS.h"
#include "task.h"
#include "FreeRTOS_POSIX/pthread.h"
#include "FreeRTOS_POSIX/time.h"
#include "diag.h"

#define IAP2_CTRL_WAIT_NS 20000000L
#define NANOSECONDS_PER_SECOND 1000000000L
/*
 * FreeRTOS+POSIX pthread_cond_timedwait() returns its own ETIMEDOUT value
 * (116), while the prebuilt iAP2Ctrl.o compares the return value with the
 * lwIP/newlib value compiled into that object (110).  Keep these numeric
 * values explicit: the visible ETIMEDOUT macro depends on header order.
 */
#define IAP2_PTHREAD_ETIMEDOUT 116
#define IAP2_VENDOR_ETIMEDOUT 110

static volatile uint32_t iap2_wait_calls;
static volatile uint32_t iap2_wait_signals;
static volatile uint32_t iap2_wait_timeouts;
static volatile uint32_t iap2_wait_errors;
static volatile int32_t iap2_wait_last_error;

/*
 * The customer iAP2Ctrl.o builds its absolute 20 ms deadline through a
 * 32-bit tv_sec * 1000 intermediate.  That is valid while gettimeofday()
 * reports uptime, but overflows after the system clock is synchronized to a
 * real Unix epoch.  Only iAP2Ctrl.o is redirected here at archive-build time;
 * all other pthread condition waits retain their original implementation.
 */
int carbox_iap2_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
			       const struct timespec *vendor_deadline)
{
	struct timespec deadline;
	int status;

	(void)vendor_deadline;
	iap2_wait_calls++;
	if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
		iap2_wait_errors++;
		iap2_wait_last_error = EINVAL;
		return EINVAL;
	}

	deadline.tv_nsec += IAP2_CTRL_WAIT_NS;
	if (deadline.tv_nsec >= NANOSECONDS_PER_SECOND) {
		deadline.tv_nsec -= NANOSECONDS_PER_SECOND;
		deadline.tv_sec++;
	}
	status = pthread_cond_timedwait(cond, mutex, &deadline);
	if (status == 0) {
		iap2_wait_signals++;
	} else if (status == IAP2_PTHREAD_ETIMEDOUT) {
		iap2_wait_timeouts++;
		status = IAP2_VENDOR_ETIMEDOUT;
	} else {
		iap2_wait_errors++;
		iap2_wait_last_error = status;
	}
	return status;
}

void carbox_iap2_cond_timedwait_fix_report(uint32_t sequence)
{
	uint32_t calls;
	uint32_t signals;
	uint32_t timeouts;
	uint32_t errors;
	int32_t last_error;
	uint32_t primask;

	primask = __get_PRIMASK();
	__disable_irq();
	calls = iap2_wait_calls;
	signals = iap2_wait_signals;
	timeouts = iap2_wait_timeouts;
	errors = iap2_wait_errors;
	last_error = iap2_wait_last_error;
	iap2_wait_calls = 0U;
	iap2_wait_signals = 0U;
	iap2_wait_timeouts = 0U;
	iap2_wait_errors = 0U;
	iap2_wait_last_error = 0;
	if (primask == 0U) {
		__enable_irq();
	}

	rt_printf("[IAP2WAITFIX][%lu] calls/signal/timeout/error=%lu/%lu/%lu/%lu "
		  "last_error=%ld timeout_ms=20\r\n",
		  (unsigned long)sequence, (unsigned long)calls,
		  (unsigned long)signals, (unsigned long)timeouts,
		  (unsigned long)errors, (long)last_error);
}

#else

void carbox_iap2_cond_timedwait_fix_report(uint32_t sequence)
{
	(void)sequence;
}

#endif
