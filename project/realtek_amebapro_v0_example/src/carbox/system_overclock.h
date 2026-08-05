#ifndef CARBOX_SYSTEM_OVERCLOCK_H
#define CARBOX_SYSTEM_OVERCLOCK_H

#ifdef __cplusplus
extern "C" {
#endif

enum carbox_system_overclock_status {
	CARBOX_OVERCLOCK_DISABLED = 0,
	CARBOX_OVERCLOCK_APPLIED = 1,
	CARBOX_OVERCLOCK_BAD_BOOT_CLOCK = -1,
	CARBOX_OVERCLOCK_BAD_PLL_STATE = -2,
	CARBOX_OVERCLOCK_LOCK_TIMEOUT = -3,
	CARBOX_OVERCLOCK_RESTORE_FAILED = -4,
	CARBOX_OVERCLOCK_STOP_TIMEOUT = -5
};

/*
 * Must be called before console, RTOS or peripheral initialization.  The
 * implementation is linked into internal SRAM because changing SYS PLL while
 * executing from the serial-flash XIP window is unsafe.
 */
int carbox_system_overclock_early(void);

#ifdef __cplusplus
}
#endif

#endif
