#ifndef CARBOX_SYSTEM_OVERCLOCK_H
#define CARBOX_SYSTEM_OVERCLOCK_H

#include <stdint.h>

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
	CARBOX_OVERCLOCK_STOP_TIMEOUT = -5,
	CARBOX_OVERCLOCK_SPIC_PREPARE_FAILED = -6,
	CARBOX_OVERCLOCK_SPIC_CALIBRATION_FAILED = -7,
	CARBOX_OVERCLOCK_CLOCK_VERIFY_FAILED = -8
};

struct carbox_system_overclock_report {
	uint32_t target_hz;
	uint32_t pll_target_hz;
	uint32_t measured_hz;
	uint32_t rollback_measured_hz;
	uint32_t cycles;
	uint32_t reference_us;
	uint8_t divn;
	uint8_t fon;
	uint16_t fof;
	uint8_t tolerance_pct;
	uint8_t measurement_valid;
	uint8_t rolled_back;
};

enum carbox_sysclk_probe_status {
	CARBOX_SYSCLK_PROBE_DISABLED = 0,
	CARBOX_SYSCLK_PROBE_PASSED = 1,
	CARBOX_SYSCLK_PROBE_BAD_BOOT_CLOCK = -1,
	CARBOX_SYSCLK_PROBE_SWITCH_FAILED = -2,
	CARBOX_SYSCLK_PROBE_RESTORE_FAILED = -3
};

struct carbox_sysclk_probe_sample {
	uint32_t requested_hz;
	uint32_t measured_hz;
	uint32_t clk_ctrl1;
	uint32_t cycles;
	uint32_t reference_us;
};

struct carbox_sysclk_probe_report {
	int32_t status;
	uint8_t restored;
	struct carbox_sysclk_probe_sample before;
	struct carbox_sysclk_probe_sample switched;
	struct carbox_sysclk_probe_sample restored_sample;
};

enum carbox_pll_isolated_probe_status {
	CARBOX_PLL_ISOLATED_PROBE_DISABLED = 0,
	CARBOX_PLL_ISOLATED_PROBE_PASSED = 1,
	CARBOX_PLL_ISOLATED_PROBE_PRESET_IGNORED = 2,
	CARBOX_PLL_ISOLATED_PROBE_BAD_BOOT_CLOCK = -20,
	CARBOX_PLL_ISOLATED_PROBE_ANA_SWITCH_FAILED = -21,
	CARBOX_PLL_ISOLATED_PROBE_NOT_READY = -22,
	CARBOX_PLL_ISOLATED_PROBE_TARGET_VERIFY_FAILED = -23,
	CARBOX_PLL_ISOLATED_PROBE_RESTORE_FAILED = -24
};

struct carbox_pll_isolated_probe_report {
	int32_t status;
	uint32_t target_pll_hz;
	uint32_t expected_div2_hz;
	uint32_t original_clk_ctrl1;
	uint32_t original_pll_ctrl1;
	uint32_t original_pll_ctrl3;
	uint32_t original_pll_ctrl0;
	uint32_t disabled_pll_ctrl0;
	uint32_t target_pll_ctrl0;
	uint32_t restored_pll_ctrl0;
	uint32_t target_pll_ctrl1;
	uint32_t target_pll_ctrl3;
	uint32_t target_pll_test;
	uint8_t divn;
	uint8_t fon;
	uint16_t fof;
	uint8_t target_ready;
	uint8_t restored;
	uint8_t power_cycle;
	uint8_t target_ready_dropped;
	uint8_t restore_ready_dropped;
	uint8_t sdm_power_after_enable;
	uint8_t manual_mode_transition;
	uint8_t original_freq_sel;
	uint8_t target_freq_sel;
	uint8_t target_direct;
	struct carbox_sysclk_probe_sample before;
	struct carbox_sysclk_probe_sample ana;
	struct carbox_sysclk_probe_sample baseline_div2;
	struct carbox_sysclk_probe_sample target_div2;
	struct carbox_sysclk_probe_sample restored_sample;
};

/*
 * Must be called before console, RTOS or peripheral initialization.  The
 * implementation is linked into internal SRAM because changing SYS PLL while
 * executing from the serial-flash XIP window is unsafe.
 */
int carbox_system_overclock_early(void);
void carbox_system_overclock_get_report(
	struct carbox_system_overclock_report *report);

/*
 * One-shot, SRAM-resident characterization of the documented ROM SYS_CLK
 * path.  It switches 300 -> 200 -> 300 MHz with interrupts masked and always
 * attempts to restore the exact boot configuration before returning.
 */
int carbox_sysclk_probe_early(void);
void carbox_sysclk_probe_get_report(struct carbox_sysclk_probe_report *report);

/*
 * Characterize an arbitrary PLL_SYS request without executing from that PLL
 * while it is programmed or restored.  The CPU is moved to the independent
 * ANA 4 MHz clock, the candidate is observed through the documented /2 path,
 * then the exact boot PLL image and 300 MHz clock path are restored.
 */
int carbox_pll_isolated_probe_early(void);
void carbox_pll_isolated_probe_get_report(
	struct carbox_pll_isolated_probe_report *report);

#ifdef __cplusplus
}
#endif

#endif
