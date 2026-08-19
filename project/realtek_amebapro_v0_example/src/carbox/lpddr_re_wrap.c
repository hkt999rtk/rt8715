#include "lpddr_re_wrap.h"

#include "hal_lpddr.h"
#include "rtl8195bhp_dram.h"
#include "rtl8195bhp_dpi.h"
#include "rtl8195bhp_lpddr.h"
#include "rtl8195bhp_lpddr_ctrl.h"

#ifndef CONFIG_LPDDR_RE_OBSERVE
#define CONFIG_LPDDR_RE_OBSERVE 0
#endif

#ifndef CONFIG_LPDDR_RE_CLOCK_HZ
#define CONFIG_LPDDR_RE_CLOCK_HZ 0U
#endif

#ifndef CONFIG_LPDDR_RE_PHASE_OVERRIDE
#define CONFIG_LPDDR_RE_PHASE_OVERRIDE 0
#endif
#ifndef CONFIG_LPDDR_RE_PHASE_CK
#define CONFIG_LPDDR_RE_PHASE_CK 14
#endif
#ifndef CONFIG_LPDDR_RE_PHASE_DQS
#define CONFIG_LPDDR_RE_PHASE_DQS 16
#endif
#ifndef CONFIG_LPDDR_RE_PHASE_DQ
#define CONFIG_LPDDR_RE_PHASE_DQ 36
#endif
#ifndef CONFIG_LPDDR_RE_PHASE_WDQS
#define CONFIG_LPDDR_RE_PHASE_WDQS 16
#endif
#ifndef CONFIG_LPDDR_RE_PHASE_WDQ
#define CONFIG_LPDDR_RE_PHASE_WDQ 36
#endif

#if CONFIG_LPDDR_RE_PHASE_OVERRIDE && \
	(CONFIG_LPDDR_RE_PHASE_WDQS != CONFIG_LPDDR_RE_PHASE_DQS)
#error "LPDDR WDQS selector must use the same value as the DQS PI phase"
#endif
#if CONFIG_LPDDR_RE_PHASE_OVERRIDE && \
	(CONFIG_LPDDR_RE_PHASE_WDQ != CONFIG_LPDDR_RE_PHASE_DQ)
#error "LPDDR WDQ selector must use the same value as the DQ PI phase"
#endif

#if CONFIG_LPDDR_RE_OBSERVE

/* These are vendor defaults in lib_soc_is.a.  They are observed only. */
extern lpddr_device_info_t lpddr_device_info;

void lpddr_phy_init(lpddr_device_info_t *device_info);
void lpddr_ctrl_init(uint32_t init_only, lpddr_device_info_t *device_info);
void __real_dram_init_clk_frequency(uint32_t dram_period_ps);
void __real_dram_set_oesync_ck(int32_t value);
void __real_dram_set_oesync_dqs(int32_t value);
void __real_dram_set_oesync_dq(int32_t value);
void __real_dram_set_wrlvl_dqs(int32_t value);
void __real_dram_set_wrlvl_dq(int32_t value);

/* application.is.mk builds this file through CINIT_C and prefixes every
 * section with .cinit.  Keep the source sections ordinary so they become
 * .cinit.text.* and .cinit.data.* exactly once. */
#define LPDDR_RE_CINIT_TEXT __attribute__((noinline))
#define LPDDR_RE_CINIT_DATA __attribute__((aligned(4)))

enum lpddr_re_event {
	LPDDR_RE_INIT_ENTER = 1,
	LPDDR_RE_CLOCK = 2,
	LPDDR_RE_OESYNC_CK = 3,
	LPDDR_RE_OESYNC_DQS = 4,
	LPDDR_RE_OESYNC_DQ = 5,
	LPDDR_RE_WRLVL_DQS = 6,
	LPDDR_RE_WRLVL_DQ = 7,
	LPDDR_RE_INIT_EXIT = 8,
};

static volatile struct carbox_lpddr_re_record lpddr_re_record
	LPDDR_RE_CINIT_DATA = {
		.magic = CARBOX_LPDDR_RE_MAGIC,
		.version = CARBOX_LPDDR_RE_VERSION,
	};

static LPDDR_RE_CINIT_TEXT void lpddr_re_event(uint32_t event)
{
	lpddr_re_record.call_order =
		(lpddr_re_record.call_order << 4) | (event & 0x0fU);
}

static LPDDR_RE_CINIT_TEXT void lpddr_re_snapshot_dpi(
	volatile uint32_t values[7])
{
	values[0] = DPI->crt_ctl;
	values[1] = DPI->pll_ctl0;
	values[2] = DPI->pll_ctl1;
	values[3] = DPI->pll_ctl3;
	values[4] = DPI->ssc0;
	values[5] = DPI->ssc1;
	values[6] = DPI->ssc2;
}

static LPDDR_RE_CINIT_TEXT void lpddr_re_snapshot_ctrl(void)
{
	lpddr_re_record.ctrl_after[0] = LPDDR_CTRL->ccr;
	lpddr_re_record.ctrl_after[1] = LPDDR_CTRL->dcr;
	lpddr_re_record.ctrl_after[2] = LPDDR_CTRL->iocr;
	lpddr_re_record.ctrl_after[3] = LPDDR_CTRL->csr;
	lpddr_re_record.ctrl_after[4] = LPDDR_CTRL->drr;
	lpddr_re_record.ctrl_after[5] = LPDDR_CTRL->tpr0;
	lpddr_re_record.ctrl_after[6] = LPDDR_CTRL->tpr1;
	lpddr_re_record.ctrl_after[7] = LPDDR_CTRL->tpr2;
	lpddr_re_record.ctrl_after[8] = LPDDR_CTRL->tpr3;
	lpddr_re_record.ctrl_after[9] = LPDDR_CTRL->mr_info;
	lpddr_re_record.ctrl_after[10] = LPDDR_CTRL->mr0;
	lpddr_re_record.ctrl_after[11] = LPDDR_CTRL->mr1;
}

static LPDDR_RE_CINIT_TEXT void lpddr_re_snapshot_phase(
	volatile uint32_t values[4])
{
	values[0] = DPI->pll_pi0;
	values[1] = DPI->pll_pi1;
	values[2] = DPI->pll_pi2;
	values[3] = DPI->wrlvl_ctrl;
}

static LPDDR_RE_CINIT_TEXT uint32_t lpddr_re_field6(uint32_t value,
	uint32_t shift, uint32_t phase)
{
	const uint32_t mask = 0x3fU << shift;

	return (value & ~mask) | ((phase & 0x3fU) << shift);
}

static LPDDR_RE_CINIT_TEXT void lpddr_re_apply_phase(void)
{
#if CONFIG_LPDDR_RE_PHASE_OVERRIDE
	/* Gate only mck_dq_0/1 (selectors 6 and 7).  Stopping CK, CMD or DQS
	 * after vendor PHY initialization can destroy the established LPDDR
	 * state even though the controller init has not run yet. */
	const uint32_t output_mask = (BIT(6) | BIT(7)) << 16;
	uint32_t ctl0;
	uint32_t ctl1;
	uint32_t pi0;
	uint32_t pi1;
	uint32_t pi2;
	const uint32_t ck = CONFIG_LPDDR_RE_PHASE_CK;
	const uint32_t dqs = CONFIG_LPDDR_RE_PHASE_DQS;
	const uint32_t dq = CONFIG_LPDDR_RE_PHASE_DQ;

	/* lpddr_phy_init() writes the true 6-bit phase values directly, so the
	 * --wrap hooks around its helper calls cannot change them.  Reapply only
	 * the x16 lanes used by the vendor routine while the controller is still
	 * inactive, preserving command, inactive lanes and internal-PLL fields. */
	/* The vendor gates mck outputs before changing PI selectors, then restores
	 * them after programming.  A live register readback alone does not prove
	 * that the analog PI sampled a new value.  Reproduce the latch edge only
	 * for the DQ lanes whose phase is changed, leaving CK/CMD/DQS continuous. */
	ctl0 = DPI->pll_ctl0;
	ctl1 = DPI->pll_ctl1;
	DPI->pll_ctl0 = ctl0 & ~output_mask;
	DPI->pll_ctl1 = ctl1 & ~output_mask;
	lpddr_re_record.phase_gate_off[0] = DPI->pll_ctl0;
	lpddr_re_record.phase_gate_off[1] = DPI->pll_ctl1;

	pi0 = DPI->pll_pi0;
	pi0 = lpddr_re_field6(pi0, 0U, ck);       /* mck_ck */
	pi0 = lpddr_re_field6(pi0, 16U, dqs);    /* mck_dqs_0 */
	pi0 = lpddr_re_field6(pi0, 24U, dqs);    /* mck_dqs_1 */
	DPI->pll_pi0 = pi0;

	pi1 = DPI->pll_pi1;
	pi1 = lpddr_re_field6(pi1, 16U, dq);     /* mck_dq_0 */
	DPI->pll_pi1 = pi1;

	pi2 = DPI->pll_pi2;
	pi2 = lpddr_re_field6(pi2, 0U, dq);      /* mck_dq_1 */
	DPI->pll_pi2 = pi2;

	/* These helpers program the matching coarse OE-sync and write-level
	 * feedback selectors.  They must follow the same phase values; WDQ/WDQS
	 * are not independent physical PI phase controls. */
	__real_dram_set_oesync_ck((int32_t)ck);
	__real_dram_set_oesync_dqs((int32_t)dqs);
	__real_dram_set_oesync_dq((int32_t)dq);
	__real_dram_set_wrlvl_dqs((int32_t)dqs);
	__real_dram_set_wrlvl_dq((int32_t)dq);

	DPI->pll_ctl0 = (DPI->pll_ctl0 & ~output_mask) |
		(ctl0 & output_mask);
	DPI->pll_ctl1 = (DPI->pll_ctl1 & ~output_mask) |
		(ctl1 & output_mask);
	lpddr_re_record.phase_gate_restored[0] = DPI->pll_ctl0;
	lpddr_re_record.phase_gate_restored[1] = DPI->pll_ctl1;
	lpddr_re_record.phase_reapply_calls++;
#endif
}

static LPDDR_RE_CINIT_TEXT uint32_t lpddr_re_target_period_ps(
	uint32_t vendor_period_ps)
{
#if CONFIG_LPDDR_RE_CLOCK_HZ > 0U
	return (uint32_t)((1000000000000ULL +
		(CONFIG_LPDDR_RE_CLOCK_HZ / 2U)) / CONFIG_LPDDR_RE_CLOCK_HZ);
#else
	return vendor_period_ps;
#endif
}

LPDDR_RE_CINIT_TEXT void __wrap_hal_lpddr_init(void)
{
	uint32_t vendor_period_ps = lpddr_device_info.ddr_period_ps;
	uint32_t applied_period_ps =
		lpddr_re_target_period_ps(vendor_period_ps);

	lpddr_re_record.complete = 0U;
	lpddr_re_record.call_order = 0U;
	lpddr_re_record.phase_calls = 0U;
	lpddr_re_record.phy_init_calls = 0U;
	lpddr_re_record.pll_reapply_calls = 0U;
	lpddr_re_record.phase_reapply_calls = 0U;
	lpddr_re_record.phase_gate_off[0] = 0U;
	lpddr_re_record.phase_gate_off[1] = 0U;
	lpddr_re_record.phase_gate_restored[0] = 0U;
	lpddr_re_record.phase_gate_restored[1] = 0U;
	lpddr_re_record.phase_override = CONFIG_LPDDR_RE_PHASE_OVERRIDE;
	lpddr_re_record.init_calls++;
	lpddr_re_record.vendor_period_ps = vendor_period_ps;
	lpddr_re_record.dram_period_ps = applied_period_ps;
	lpddr_re_record.clock_override_hz = CONFIG_LPDDR_RE_CLOCK_HZ;
	lpddr_re_record.device = (uint32_t)lpddr_device_info.pdev;
	lpddr_re_record.mode = (uint32_t)lpddr_device_info.pmode_reg;
	lpddr_re_record.timing = (uint32_t)lpddr_device_info.ptiming;
	lpddr_re_event(LPDDR_RE_INIT_ENTER);
	/* Do not read DPI before the vendor routine enables its clock and power.
	 * An early APB read can fault before the LPDDR PHY is accessible. */
	/* The controller derives its cycle counts from this same period.  Updating
	 * only dram_init_clk_frequency() would overclock the PHY while retaining
	 * the vendor 200 MHz controller timings.  Keep the applied period in the
	 * global descriptor for later LPDDR resume paths as well. */
	lpddr_device_info.ddr_period_ps = applied_period_ps;

	/* Vendor hal_lpddr_init() is exactly a PHY init followed by controller
	 * init.  Those two routines live in the same archive object, so GNU
	 * --wrap cannot intercept their internal call.  Keep that ordering here
	 * and insert the PLL reapply while the controller is still inactive. */
	lpddr_phy_init(&lpddr_device_info);
	lpddr_re_record.phy_init_calls++;
	lpddr_re_snapshot_dpi(lpddr_re_record.dpi_before);
	lpddr_re_snapshot_phase(lpddr_re_record.phase_before);
#if CONFIG_LPDDR_RE_CLOCK_HZ > 0U
	__real_dram_init_clk_frequency(applied_period_ps);
	lpddr_re_record.pll_reapply_calls++;
#endif
	lpddr_re_apply_phase();
	lpddr_re_snapshot_dpi(lpddr_re_record.dpi_after);
	lpddr_re_snapshot_phase(lpddr_re_record.phase_after);
	lpddr_ctrl_init(0U, &lpddr_device_info);

	lpddr_re_snapshot_ctrl();
	lpddr_re_event(LPDDR_RE_INIT_EXIT);
	lpddr_re_record.complete = 1U;
}

LPDDR_RE_CINIT_TEXT void __wrap_dram_init_clk_frequency(uint32_t period_ps)
{
	uint32_t applied_period_ps = lpddr_re_target_period_ps(period_ps);

	lpddr_re_record.dram_period_ps = applied_period_ps;
	lpddr_re_event(LPDDR_RE_CLOCK);
	__real_dram_init_clk_frequency(applied_period_ps);
}

LPDDR_RE_CINIT_TEXT void __wrap_dram_set_oesync_ck(int32_t value)
{
#if CONFIG_LPDDR_RE_PHASE_OVERRIDE
	value = CONFIG_LPDDR_RE_PHASE_CK;
#endif
	lpddr_re_record.oesync_ck = value;
	lpddr_re_record.phase_calls++;
	lpddr_re_event(LPDDR_RE_OESYNC_CK);
	__real_dram_set_oesync_ck(value);
}

LPDDR_RE_CINIT_TEXT void __wrap_dram_set_oesync_dqs(int32_t value)
{
#if CONFIG_LPDDR_RE_PHASE_OVERRIDE
	value = CONFIG_LPDDR_RE_PHASE_DQS;
#endif
	lpddr_re_record.oesync_dqs = value;
	lpddr_re_record.phase_calls++;
	lpddr_re_event(LPDDR_RE_OESYNC_DQS);
	__real_dram_set_oesync_dqs(value);
}

LPDDR_RE_CINIT_TEXT void __wrap_dram_set_oesync_dq(int32_t value)
{
#if CONFIG_LPDDR_RE_PHASE_OVERRIDE
	value = CONFIG_LPDDR_RE_PHASE_DQ;
#endif
	lpddr_re_record.oesync_dq = value;
	lpddr_re_record.phase_calls++;
	lpddr_re_event(LPDDR_RE_OESYNC_DQ);
	__real_dram_set_oesync_dq(value);
}

LPDDR_RE_CINIT_TEXT void __wrap_dram_set_wrlvl_dqs(int32_t value)
{
#if CONFIG_LPDDR_RE_PHASE_OVERRIDE
	value = CONFIG_LPDDR_RE_PHASE_WDQS;
#endif
	lpddr_re_record.wrlvl_dqs = value;
	lpddr_re_record.phase_calls++;
	lpddr_re_event(LPDDR_RE_WRLVL_DQS);
	__real_dram_set_wrlvl_dqs(value);
}

LPDDR_RE_CINIT_TEXT void __wrap_dram_set_wrlvl_dq(int32_t value)
{
#if CONFIG_LPDDR_RE_PHASE_OVERRIDE
	value = CONFIG_LPDDR_RE_PHASE_WDQ;
#endif
	lpddr_re_record.wrlvl_dq = value;
	lpddr_re_record.phase_calls++;
	lpddr_re_event(LPDDR_RE_WRLVL_DQ);
	__real_dram_set_wrlvl_dq(value);
}

LPDDR_RE_CINIT_TEXT const volatile struct carbox_lpddr_re_record *
carbox_lpddr_re_get_record(void)
{
	return &lpddr_re_record;
}

#else

const volatile struct carbox_lpddr_re_record *
carbox_lpddr_re_get_record(void)
{
	return (const volatile struct carbox_lpddr_re_record *)0;
}

#endif
