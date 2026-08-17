#ifndef CARBOX_SPIC_OVERCLOCK_H
#define CARBOX_SPIC_OVERCLOCK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum carbox_spic_overclock_status {
	CARBOX_SPIC_OVERCLOCK_IDLE = 0,
	CARBOX_SPIC_OVERCLOCK_PREPARED = 1,
	CARBOX_SPIC_OVERCLOCK_CALIBRATED = 2,
	CARBOX_SPIC_OVERCLOCK_UNAVAILABLE = -1,
	CARBOX_SPIC_OVERCLOCK_BAD_CONFIG = -2,
	CARBOX_SPIC_OVERCLOCK_NO_WINDOW = -3,
	CARBOX_SPIC_OVERCLOCK_VERIFY_FAILED = -4
};

struct carbox_spic_overclock_report {
	int32_t status;
	uint32_t target_sys_hz;
	uint32_t qualified_max_hz;
	uint32_t selected_spic_hz;
	uint8_t flash_id[3];
	uint8_t io_mode;
	uint8_t old_divider;
	uint8_t selected_divider;
	uint8_t selected_dummy;
	uint8_t selected_delay;
	uint8_t delay_window;
	uint8_t sequential_was_enabled;
	uint8_t sequential_is_enabled;
};

/*
 * These functions execute while serial-flash XIP is unsafe.  Their complete
 * implementation and call graph must remain in internal SRAM/ROM.
 */
int carbox_spic_overclock_prepare(uint32_t target_sys_hz);
int carbox_spic_overclock_calibrate(uint32_t target_sys_hz);
void carbox_spic_overclock_restore(void);
void carbox_spic_overclock_get_report(struct carbox_spic_overclock_report *report);

#ifdef __cplusplus
}
#endif

#endif
