#include "FreeRTOS.h"
#include "task.h"

extern void *GetAirPlayInfo(void);
extern int AirPlayInfo_IsPaired(void);
extern int AirPlayInfo_GetFailNum(void);

enum pair_fastpath_state {
	PAIR_FASTPATH_NEW,
	PAIR_FASTPATH_VERIFY,
	PAIR_FASTPATH_SETUP_FALLBACK,
	PAIR_FASTPATH_SETUP_RETRY,
	PAIR_FASTPATH_LEGACY,
};

static TaskHandle_t pair_fastpath_owner;
static enum pair_fastpath_state pair_fastpath_state;

/* Routed from SendMessagesNormal's first GetAirPlayInfo call only. */
void *carbox_pair_fastpath_begin(void)
{
	pair_fastpath_owner = xTaskGetCurrentTaskHandle();
	pair_fastpath_state = PAIR_FASTPATH_NEW;
	return GetAirPlayInfo();
}

/* Routed from the setup/verify selection site only. */
int carbox_pair_fastpath_is_paired(void)
{
	int paired;

	if (pair_fastpath_owner != xTaskGetCurrentTaskHandle()) {
		return 0;
	}
	if (pair_fastpath_state == PAIR_FASTPATH_VERIFY) {
		/* Verify failed; perform the original setup on this retry. */
		pair_fastpath_state = PAIR_FASTPATH_SETUP_FALLBACK;
		return 0;
	}
	if (pair_fastpath_state == PAIR_FASTPATH_SETUP_FALLBACK ||
	    pair_fastpath_state == PAIR_FASTPATH_SETUP_RETRY) {
		return 0;
	}
	paired = AirPlayInfo_IsPaired();
	if (pair_fastpath_state == PAIR_FASTPATH_NEW) {
		pair_fastpath_state = paired ? PAIR_FASTPATH_VERIFY : PAIR_FASTPATH_LEGACY;
	}
	return paired;
}

int carbox_pair_fastpath_fail_num(void)
{
	if (pair_fastpath_owner == xTaskGetCurrentTaskHandle() &&
	    pair_fastpath_state == PAIR_FASTPATH_SETUP_FALLBACK) {
		/* A rejected fast path must still be allowed to create a fresh pair. */
		pair_fastpath_state = PAIR_FASTPATH_SETUP_RETRY;
		return 0;
	}
	return AirPlayInfo_GetFailNum();
}
