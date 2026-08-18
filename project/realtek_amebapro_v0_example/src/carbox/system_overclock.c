#include <stdint.h>

#include "cmsis.h"
#include "hal_syson.h"
#include "system_overclock.h"
#include "spic_overclock.h"

#ifndef CONFIG_SYS_PLL_OVERCLOCK
#define CONFIG_SYS_PLL_OVERCLOCK 0
#endif

#ifndef CONFIG_SYS_PLL_TARGET_HZ
#define CONFIG_SYS_PLL_TARGET_HZ 300000000UL
#endif

#ifndef CONFIG_SYS_CLK_SWITCH_PROBE
#define CONFIG_SYS_CLK_SWITCH_PROBE 0
#endif

#ifndef CONFIG_SYS_PLL_ISOLATED_PROBE
#define CONFIG_SYS_PLL_ISOLATED_PROBE 0
#endif

#ifndef CONFIG_SYS_PLL_ISOLATED_TARGET_HZ
#define CONFIG_SYS_PLL_ISOLATED_TARGET_HZ 310000000UL
#endif

#ifndef CONFIG_SYS_PLL_ISOLATED_POWER_CYCLE
#define CONFIG_SYS_PLL_ISOLATED_POWER_CYCLE 0
#endif

#ifndef CONFIG_SYS_PLL_ISOLATED_TRIGGER_HOLD
#define CONFIG_SYS_PLL_ISOLATED_TRIGGER_HOLD 0
#endif

#ifndef CONFIG_SYS_PLL_ISOLATED_SDM_POWER
#define CONFIG_SYS_PLL_ISOLATED_SDM_POWER 0
#endif

#ifndef CONFIG_SYS_PLL_ISOLATED_MANUAL_MODE
#define CONFIG_SYS_PLL_ISOLATED_MANUAL_MODE 0
#endif

#ifndef CONFIG_SYS_PLL_ISOLATED_FREQ_SEL
#define CONFIG_SYS_PLL_ISOLATED_FREQ_SEL 1
#endif

#ifndef CONFIG_SYS_PLL_ISOLATED_DIRECT
#define CONFIG_SYS_PLL_ISOLATED_DIRECT 0
#endif

#define CARBOX_SYS_PLL_NOMINAL_HZ       300000000UL
#define CARBOX_SYS_PLL_INPUT_HZ          40000000UL
#define CARBOX_SYS_PLL_DIVN_BIAS                 2UL

#define CARBOX_SYSON_BASE               0x40000000UL
#define CARBOX_SYS_CLK_CTRL1             0x00000014UL
#define CARBOX_PLL_SYS_CTRL0             0x00000050UL
#define CARBOX_PLL_SYS_CTRL1             0x00000054UL
#define CARBOX_PLL_SYS_CTRL3             0x0000005CUL
#define CARBOX_PLL_TEST                  0x000000A0UL

#define CARBOX_SYS_CLK_PLL_300M          (0UL << 0)
#define CARBOX_SYS_CLK_SOURCE_PLL        (1UL << 8)
#define CARBOX_SYS_CLK_DIV_ENABLE        (1UL << 9)
#define CARBOX_SYS_CLK_PLL_DIV_MASK      (0xFUL << 4)

#define CARBOX_PLL_AUTO_MODE             (1UL << 2)
#define CARBOX_PLL_CLK_ENABLE            (1UL << 8)
#define CARBOX_PLL_ENABLE                (1UL << 29)
#define CARBOX_PLL_INPUT_DIV_ENABLE      (1UL << 30)
#define CARBOX_PLL_POWCUT_ENABLE         (1UL << 31)
#define CARBOX_PLL_READY                 (1UL << 26)

#define CARBOX_PLL_DIVN_SHIFT                    5U
#define CARBOX_PLL_SDM_FCODE_POWER        (1UL << 0)
#define CARBOX_PLL_DIVN_MASK             (0x3FUL << CARBOX_PLL_DIVN_SHIFT)
#define CARBOX_PLL_TRIGGER_REQUEST        (1UL << 15)
#define CARBOX_PLL_FREQ_SEL_SHIFT                24U
#define CARBOX_PLL_FREQ_SEL_MASK          (0xFUL << CARBOX_PLL_FREQ_SEL_SHIFT)
#define CARBOX_PLL_XTAL_SHIFT                   28U
#define CARBOX_PLL_XTAL_MASK             (0xFUL << CARBOX_PLL_XTAL_SHIFT)
#define CARBOX_PLL_FON_SHIFT                    16U
#define CARBOX_PLL_FON_MASK              (0x7UL << CARBOX_PLL_FON_SHIFT)
#define CARBOX_PLL_FOF_SHIFT                    19U
#define CARBOX_PLL_FOF_MASK              (0x1FFFUL << CARBOX_PLL_FOF_SHIFT)

#define CARBOX_PLL_FRAC_BITS                    16U
#define CARBOX_PLL_FON_TO_FIXED_SHIFT           13U
#define CARBOX_PLL_SETTLE_LOOPS              300000U
#define CARBOX_CLOCK_VERIFY_US                 50000U
#define CARBOX_CLOCK_VERIFY_TOLERANCE_PCT          2U
#define CARBOX_CLOCK_VERIFY_SAMPLE_LIMIT      1000000U

#define CARBOX_SYSTIMER_BASE             0x40002000UL
#define CARBOX_SYSTIMER_LATCH_OFFSET            0x08UL
#define CARBOX_SYSTIMER_COUNT_OFFSET            0x0CUL
#define CARBOX_SYSTIMER_LATCH_BIT                0x80UL
#define CARBOX_SYSTIMER_LATCH_LOOPS            100000U

extern uint32_t SystemCoreClock;

static struct carbox_system_overclock_report carbox_overclock_report;
static struct carbox_sysclk_probe_report carbox_sysclk_probe_report;
static struct carbox_pll_isolated_probe_report carbox_pll_isolated_report;

static volatile uint32_t *carbox_syson_reg(uint32_t offset)
{
	return (volatile uint32_t *)(CARBOX_SYSON_BASE + offset);
}

static void carbox_clock_barrier(void)
{
	__asm volatile("dsb 0xf\n\tisb 0xf" ::: "memory");
}

static uint32_t carbox_irq_save(void)
{
	uint32_t primask;

	__asm volatile("mrs %0, primask\n\tcpsid i" : "=r" (primask) :: "memory");
	return primask;
}

static void carbox_irq_restore(uint32_t primask)
{
	__asm volatile("msr primask, %0" :: "r" (primask) : "memory");
}

static void carbox_pll_settle_delay(void)
{
	volatile uint32_t i;

	for (i = 0; i < CARBOX_PLL_SETTLE_LOOPS; ++i) {
		__asm volatile("nop");
	}
}

static void carbox_pll_powerup_delay(void)
{
	volatile uint32_t i;

	/* Vendor PLL enable paths wait at least 1 us after releasing power cut. */
	for (i = 0U; i < 16U; ++i) {
		__asm volatile("nop");
	}
}

static uint32_t carbox_systimer_read_us(void)
{
	volatile uint32_t *latch = (volatile uint32_t *)(CARBOX_SYSTIMER_BASE +
		CARBOX_SYSTIMER_LATCH_OFFSET);
	volatile uint32_t *count = (volatile uint32_t *)(CARBOX_SYSTIMER_BASE +
		CARBOX_SYSTIMER_COUNT_OFFSET);
	uint32_t i;

	*latch = CARBOX_SYSTIMER_LATCH_BIT;
	for (i = 0U; i < CARBOX_SYSTIMER_LATCH_LOOPS; ++i) {
		if ((*latch & CARBOX_SYSTIMER_LATCH_BIT) == 0U) {
			return *count;
		}
	}
	return *count;
}

static uint32_t carbox_clock_reference_sample(uint32_t *cycle_midpoint)
{
	uint32_t before = DWT->CYCCNT;
	uint32_t reference = carbox_systimer_read_us();
	uint32_t after = DWT->CYCCNT;

	*cycle_midpoint = before + ((after - before) / 2U);
	return reference;
}

static void carbox_clock_measure_raw(uint32_t *cycles_out,
				      uint32_t *reference_us_out)
{
	uint32_t cycle_start;
	uint32_t cycle_end;
	uint32_t ref_start;
	uint32_t ref_end;
	uint32_t reference_us;
	uint32_t cycles;
	uint32_t samples;

	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	ref_start = carbox_clock_reference_sample(&cycle_start);
	for (samples = 0U; samples < CARBOX_CLOCK_VERIFY_SAMPLE_LIMIT; ++samples) {
		ref_end = carbox_clock_reference_sample(&cycle_end);
		if ((ref_end - ref_start) >= CARBOX_CLOCK_VERIFY_US) {
			break;
		}
	}

	reference_us = ref_end - ref_start;
	cycles = cycle_end - cycle_start;
	*cycles_out = cycles;
	*reference_us_out = reference_us;
}

static uint32_t carbox_clock_calculate_hz(uint32_t cycles,
					  uint32_t reference_us)
{
	if ((reference_us < (CARBOX_CLOCK_VERIFY_US / 2U)) || (cycles == 0U)) {
		return 0U;
	}
	return (uint32_t)(((uint64_t)cycles * 1000000ULL +
			   (reference_us / 2U)) / reference_us);
}

static uint32_t carbox_clock_measure_hz(void)
{
	uint32_t cycles;
	uint32_t reference_us;

	carbox_clock_measure_raw(&cycles, &reference_us);
	carbox_overclock_report.reference_us = reference_us;
	carbox_overclock_report.cycles = cycles;
	return carbox_clock_calculate_hz(cycles, reference_us);
}

static int carbox_clock_measurement_matches(uint32_t measured_hz,
					     uint32_t target_hz)
{
	uint32_t difference;

	if ((measured_hz == 0U) || (target_hz == 0U)) {
		return 0;
	}
	difference = measured_hz > target_hz ?
		measured_hz - target_hz : target_hz - measured_hz;
	return ((uint64_t)difference * 100ULL <=
		(uint64_t)target_hz * CARBOX_CLOCK_VERIFY_TOLERANCE_PCT);
}

struct carbox_pll_codes {
	uint32_t pll_hz;
	uint32_t divn;
	uint32_t fon;
	uint32_t fof;
};

static int carbox_pll_encode_cpu_hz(uint32_t cpu_hz,
				     struct carbox_pll_codes *codes)
{
	uint64_t pll_hz = cpu_hz;
	uint64_t divider_fixed;
	uint32_t integer_divider;

	/* SYS_CLK_CTRL1 divider selector zero is /1: CPU = PLL_SYS. */
	if ((pll_hz > 0xFFFFFFFFULL) || (codes == 0)) {
		return -1;
	}
	divider_fixed = ((pll_hz << CARBOX_PLL_FRAC_BITS) +
			 (CARBOX_SYS_PLL_INPUT_HZ / 2U)) /
			 CARBOX_SYS_PLL_INPUT_HZ;
	integer_divider = (uint32_t)(divider_fixed >> CARBOX_PLL_FRAC_BITS);
	if ((integer_divider < CARBOX_SYS_PLL_DIVN_BIAS) ||
	    ((integer_divider - CARBOX_SYS_PLL_DIVN_BIAS) > 0x3FU)) {
		return -1;
	}

	codes->pll_hz = (uint32_t)pll_hz;
	codes->divn = integer_divider - CARBOX_SYS_PLL_DIVN_BIAS;
	codes->fon = (uint32_t)((divider_fixed >>
			 CARBOX_PLL_FON_TO_FIXED_SHIFT) & 0x7U);
	codes->fof = (uint32_t)(divider_fixed & 0x1FFFU);
	return 0;
}

static void carbox_pll_request(volatile uint32_t *pll1_reg,
			       volatile uint32_t *pll3_reg,
			       uint32_t base_pll1, uint32_t base_pll3,
			       const struct carbox_pll_codes *codes)
{
	uint32_t pll1;
	uint32_t pll3;

	/*
	 * Keep the ROM's XTAL, frequency selector and ramp step/timebase fields.
	 * FREQ_SEL=0 was experimentally proven to select a 400 MHz preset, not
	 * manual SDM mode.  Program the documented SDM fields while retaining the
	 * known-good 300 MHz selector, then hold TRIG_RREQ_EN long enough for the
	 * hardware ramp engine to consume the request.  PLL_SYS is never powered
	 * down.
	 */
	pll1 = base_pll1 & ~(CARBOX_PLL_DIVN_MASK |
				 CARBOX_PLL_TRIGGER_REQUEST);
	pll1 |= codes->divn << CARBOX_PLL_DIVN_SHIFT;
	pll3 = base_pll3 & ~(CARBOX_PLL_FON_MASK | CARBOX_PLL_FOF_MASK);
	pll3 |= (codes->fon << CARBOX_PLL_FON_SHIFT) |
		(codes->fof << CARBOX_PLL_FOF_SHIFT);

	*pll3_reg = pll3;
	carbox_clock_barrier();
	*pll1_reg = pll1 | CARBOX_PLL_TRIGGER_REQUEST;
	carbox_clock_barrier();
	carbox_pll_settle_delay();
	*pll1_reg = pll1;
	carbox_clock_barrier();
}

static int carbox_pll_power_cycle_image(volatile uint32_t *pll0_reg,
					 volatile uint32_t *pll1_reg,
					 volatile uint32_t *pll3_reg,
					 volatile uint32_t *pll_test_reg,
					 uint32_t base_pll0,
					 uint32_t pll1, uint32_t pll3,
					 uint32_t *disabled_pll0,
					 uint8_t *ready_dropped);

static int carbox_pll_restore_nominal(volatile uint32_t *pll1_reg,
				       volatile uint32_t *pll3_reg,
				       uint32_t base_pll1,
				       uint32_t base_pll3)
{
	uint32_t cycles;
	uint32_t reference_us;

	/* Restore the exact ROM preset selector and calibration image. */
	*pll3_reg = base_pll3;
	carbox_clock_barrier();
	*pll1_reg = base_pll1 | CARBOX_PLL_TRIGGER_REQUEST;
	carbox_clock_barrier();
	carbox_pll_settle_delay();
	*pll1_reg = base_pll1;
	carbox_clock_barrier();
	SystemCoreClock = CARBOX_SYS_PLL_NOMINAL_HZ;
	carbox_clock_barrier();
	carbox_clock_measure_raw(&cycles, &reference_us);
	carbox_overclock_report.rollback_measured_hz =
		carbox_clock_calculate_hz(cycles, reference_us);
	carbox_overclock_report.rolled_back = 1U;
	carbox_spic_overclock_restore();

	return carbox_clock_measurement_matches(
		carbox_overclock_report.rollback_measured_hz,
		CARBOX_SYS_PLL_NOMINAL_HZ) ? 0 : -1;
}

/*
 * Roll a sustained preset experiment back while executing from SRAM.  The
 * caller must already have moved the CPU to ANA so PLL_SYS and SPIC can be
 * restored without fetching through the clock tree being changed.
 */
static int carbox_overclock_restore_300_from_ana(
	volatile uint32_t *clk_reg,
	volatile uint32_t *pll0_reg,
	volatile uint32_t *pll1_reg,
	volatile uint32_t *pll3_reg,
	volatile uint32_t *pll_test_reg,
	uint32_t original_clk,
	uint32_t original_pll0,
	uint32_t original_pll1,
	uint32_t original_pll3)
{
	uint8_t ready_dropped;
	uint32_t cycles;
	uint32_t reference_us;

	if (carbox_pll_power_cycle_image(pll0_reg, pll1_reg, pll3_reg,
			pll_test_reg, original_pll0, original_pll1,
			original_pll3, 0, &ready_dropped) != 0) {
		return -1;
	}
	carbox_spic_overclock_restore();
	*clk_reg = original_clk;
	carbox_clock_barrier();
	SystemCoreClock = CARBOX_SYS_PLL_NOMINAL_HZ;
	carbox_clock_measure_raw(&cycles, &reference_us);
	carbox_overclock_report.rollback_measured_hz =
		carbox_clock_calculate_hz(cycles, reference_us);
	carbox_overclock_report.rolled_back = 1U;

	return carbox_clock_measurement_matches(
		carbox_overclock_report.rollback_measured_hz,
		CARBOX_SYS_PLL_NOMINAL_HZ) ? 0 : -1;
}

static void carbox_pll_restore_image(volatile uint32_t *pll1_reg,
				     volatile uint32_t *pll3_reg,
				     uint32_t pll1, uint32_t pll3)
{
	*pll3_reg = pll3;
	carbox_clock_barrier();
	*pll1_reg = pll1 | CARBOX_PLL_TRIGGER_REQUEST;
	carbox_clock_barrier();
	carbox_pll_settle_delay();
	*pll1_reg = pll1;
	carbox_clock_barrier();
}

/*
 * Reconfigure PLL_SYS only while the CPU is on ANA 4 MHz and this function is
 * executing from internal SRAM.  The ordering mirrors the vendor PLL control
 * functions: gate the output, power the PLL/input divider down, program the
 * image, power up and wait for READY before exposing the output again.
 */
static int carbox_pll_power_cycle_image(volatile uint32_t *pll0_reg,
					 volatile uint32_t *pll1_reg,
					 volatile uint32_t *pll3_reg,
					 volatile uint32_t *pll_test_reg,
					 uint32_t base_pll0,
					 uint32_t pll1, uint32_t pll3,
					 uint32_t *disabled_pll0,
					 uint8_t *ready_dropped)
{
	uint32_t i;
	uint32_t off = base_pll0 & ~(CARBOX_PLL_CLK_ENABLE |
		CARBOX_PLL_ENABLE | CARBOX_PLL_INPUT_DIV_ENABLE |
		CARBOX_PLL_POWCUT_ENABLE);
	uint32_t power_released = off | CARBOX_PLL_POWCUT_ENABLE;
	uint32_t powered = (base_pll0 & ~CARBOX_PLL_CLK_ENABLE) |
		CARBOX_PLL_ENABLE | CARBOX_PLL_INPUT_DIV_ENABLE |
		CARBOX_PLL_POWCUT_ENABLE;
	uint32_t pll1_before_enable = pll1 & ~CARBOX_PLL_SDM_FCODE_POWER;

	*ready_dropped = 0U;
	*pll0_reg = base_pll0 & ~CARBOX_PLL_CLK_ENABLE;
	carbox_clock_barrier();
	*pll0_reg = off;
	carbox_clock_barrier();
	if (disabled_pll0 != 0) {
		*disabled_pll0 = *pll0_reg;
	}
	for (i = 0U; i < CARBOX_PLL_SETTLE_LOOPS; ++i) {
		if ((*pll_test_reg & CARBOX_PLL_READY) == 0U) {
			*ready_dropped = 1U;
			break;
		}
	}

	/* RTL8195B vendor PLL paths release power cut before configuration. */
	*pll0_reg = power_released;
	carbox_clock_barrier();
	carbox_pll_powerup_delay();
	*pll3_reg = pll3;
	carbox_clock_barrier();
	/* The caller decides whether the documented ramp enable remains asserted. */
	*pll1_reg = pll1_before_enable;
	carbox_clock_barrier();
	*pll0_reg = powered;
	carbox_clock_barrier();
	/* Related Realtek vendor code powers the SDM F-code after PLL enable. */
	if ((pll1 & CARBOX_PLL_SDM_FCODE_POWER) != 0U) {
		*pll1_reg = pll1;
		carbox_clock_barrier();
	}
	for (i = 0U; i < CARBOX_PLL_SETTLE_LOOPS; ++i) {
		if ((*pll_test_reg & CARBOX_PLL_READY) != 0U) {
			*pll0_reg = base_pll0;
			carbox_clock_barrier();
			return 0;
		}
	}
	return -1;
}

int carbox_pll_isolated_probe_early(void)
{
#if !CONFIG_SYS_PLL_ISOLATED_PROBE
	carbox_pll_isolated_report.status =
		CARBOX_PLL_ISOLATED_PROBE_DISABLED;
	return CARBOX_PLL_ISOLATED_PROBE_DISABLED;
#else
	volatile uint32_t *clk_reg = carbox_syson_reg(CARBOX_SYS_CLK_CTRL1);
	volatile uint32_t *pll0_reg = carbox_syson_reg(CARBOX_PLL_SYS_CTRL0);
	volatile uint32_t *pll1_reg = carbox_syson_reg(CARBOX_PLL_SYS_CTRL1);
	volatile uint32_t *pll3_reg = carbox_syson_reg(CARBOX_PLL_SYS_CTRL3);
	volatile uint32_t *pll_test_reg = carbox_syson_reg(CARBOX_PLL_TEST);
	struct carbox_pll_isolated_probe_report *report =
		&carbox_pll_isolated_report;
	struct carbox_pll_codes target;
	uint32_t original_clk = *clk_reg;
	uint32_t original_pll0 = *pll0_reg;
	uint32_t original_pll1 = *pll1_reg;
	uint32_t original_pll3 = *pll3_reg;
	uint32_t primask;
	int status = CARBOX_PLL_ISOLATED_PROBE_BAD_BOOT_CLOCK;

	report->status = status;
	report->target_pll_hz = CONFIG_SYS_PLL_ISOLATED_TARGET_HZ;
	report->expected_div2_hz = CONFIG_SYS_PLL_ISOLATED_TARGET_HZ /
		(CONFIG_SYS_PLL_ISOLATED_DIRECT ? 1U : 2U);
	report->original_clk_ctrl1 = original_clk;
	report->original_pll_ctrl1 = original_pll1;
	report->original_pll_ctrl3 = original_pll3;
	report->original_pll_ctrl0 = original_pll0;
	report->disabled_pll_ctrl0 = original_pll0;
	report->target_pll_ctrl0 = original_pll0;
	report->restored_pll_ctrl0 = original_pll0;
	report->target_pll_ctrl1 = 0U;
	report->target_pll_ctrl3 = 0U;
	report->target_pll_test = 0U;
	report->divn = 0U;
	report->fon = 0U;
	report->fof = 0U;
	report->target_ready = 0U;
	report->restored = 0U;
	report->power_cycle = CONFIG_SYS_PLL_ISOLATED_POWER_CYCLE ? 1U : 0U;
	report->target_ready_dropped = 0U;
	report->restore_ready_dropped = 0U;
	report->sdm_power_after_enable = 0U;
	report->manual_mode_transition = 0U;
	report->original_freq_sel = (uint8_t)((original_pll1 &
		CARBOX_PLL_FREQ_SEL_MASK) >> CARBOX_PLL_FREQ_SEL_SHIFT);
	report->target_freq_sel = (uint8_t)(CONFIG_SYS_PLL_ISOLATED_FREQ_SEL &
		0xFU);
	report->target_direct = CONFIG_SYS_PLL_ISOLATED_DIRECT ? 1U : 0U;
	report->before.requested_hz = CARBOX_SYS_PLL_NOMINAL_HZ;
	report->before.measured_hz = 0U;
	report->before.clk_ctrl1 = original_clk;
	report->before.cycles = 0U;
	report->before.reference_us = 0U;
	report->ana.requested_hz = SYS_CLK_4M;
	report->ana.measured_hz = 0U;
	report->ana.clk_ctrl1 = original_clk;
	report->ana.cycles = 0U;
	report->ana.reference_us = 0U;
	report->baseline_div2.requested_hz = SYS_CLK_150M;
	report->baseline_div2.measured_hz = 0U;
	report->baseline_div2.clk_ctrl1 = original_clk;
	report->baseline_div2.cycles = 0U;
	report->baseline_div2.reference_us = 0U;
	report->target_div2.requested_hz = report->expected_div2_hz;
	report->target_div2.measured_hz = 0U;
	report->target_div2.clk_ctrl1 = original_clk;
	report->target_div2.cycles = 0U;
	report->target_div2.reference_us = 0U;
	report->restored_sample.requested_hz = CARBOX_SYS_PLL_NOMINAL_HZ;
	report->restored_sample.measured_hz = 0U;
	report->restored_sample.clk_ctrl1 = original_clk;
	report->restored_sample.cycles = 0U;
	report->restored_sample.reference_us = 0U;

	if ((SystemCoreClock != CARBOX_SYS_PLL_NOMINAL_HZ) ||
	    ((original_clk & (CARBOX_SYS_CLK_SOURCE_PLL |
			      CARBOX_SYS_CLK_DIV_ENABLE |
			      CARBOX_SYS_CLK_PLL_DIV_MASK | 1UL)) !=
	     (CARBOX_SYS_CLK_SOURCE_PLL | CARBOX_SYS_CLK_DIV_ENABLE |
	      CARBOX_SYS_CLK_PLL_300M)) ||
	    ((original_pll0 & (CARBOX_PLL_ENABLE | CARBOX_PLL_CLK_ENABLE)) !=
	     (CARBOX_PLL_ENABLE | CARBOX_PLL_CLK_ENABLE)) ||
	    ((*pll_test_reg & CARBOX_PLL_READY) == 0U) ||
	    (carbox_pll_encode_cpu_hz(CONFIG_SYS_PLL_ISOLATED_TARGET_HZ,
				       &target) != 0)) {
		return status;
	}
	report->divn = (uint8_t)target.divn;
	report->fon = (uint8_t)target.fon;
	report->fof = (uint16_t)target.fof;

	primask = carbox_irq_save();
	carbox_clock_measure_raw(&report->before.cycles,
				 &report->before.reference_us);

	/* Measure the qualified 300 MHz preset through the same /2 path first. */
	hal_syson_set_sys_clk(SYS_CLK_150M);
	carbox_clock_barrier();
	report->baseline_div2.clk_ctrl1 = *clk_reg;
	carbox_clock_measure_raw(&report->baseline_div2.cycles,
				 &report->baseline_div2.reference_us);

	/* All PLL writes and restoration happen while CPU executes from ANA 4 MHz. */
	hal_syson_set_sys_clk(SYS_CLK_4M);
	carbox_clock_barrier();
	report->ana.clk_ctrl1 = *clk_reg;
	carbox_clock_measure_raw(&report->ana.cycles,
				 &report->ana.reference_us);
	if ((*clk_reg & CARBOX_SYS_CLK_SOURCE_PLL) != 0U) {
		status = CARBOX_PLL_ISOLATED_PROBE_ANA_SWITCH_FAILED;
		/* PLL was never modified, so do not touch it from an unknown source. */
		goto restore_clock_only;
	}

	if (CONFIG_SYS_PLL_ISOLATED_POWER_CYCLE) {
		uint32_t target_pll1 = original_pll1 &
			~(CARBOX_PLL_DIVN_MASK | CARBOX_PLL_TRIGGER_REQUEST);
		uint32_t target_pll3 = original_pll3 &
			~(CARBOX_PLL_FON_MASK | CARBOX_PLL_FOF_MASK);
		uint32_t startup_pll1 = target_pll1;
		uint32_t startup_pll3 = target_pll3;

		target_pll1 |= target.divn << CARBOX_PLL_DIVN_SHIFT;
		if (CONFIG_SYS_PLL_ISOLATED_TRIGGER_HOLD) {
			target_pll1 |= CARBOX_PLL_TRIGGER_REQUEST;
		}
		target_pll3 |= (target.fon << CARBOX_PLL_FON_SHIFT) |
			(target.fof << CARBOX_PLL_FOF_SHIFT);
		if (CONFIG_SYS_PLL_ISOLATED_TARGET_HZ ==
		    CARBOX_SYS_PLL_NOMINAL_HZ) {
			/* First validate a deep cycle using the exact ROM image. */
			startup_pll1 = original_pll1;
			startup_pll3 = original_pll3;
		} else if (CONFIG_SYS_PLL_ISOLATED_SDM_POWER) {
			/*
			 * The related Realtek fractional-PLL implementation names
			 * CTRL1 bit0 POW_SDM_FCODE.  Power the PLL on its preset
			 * image first, then apply the manual SDM codes with the
			 * documented request pulse, matching that vendor sequence.
			 */
			startup_pll1 = original_pll1 | CARBOX_PLL_SDM_FCODE_POWER;
			startup_pll3 = original_pll3;
		}
		if (carbox_pll_power_cycle_image(pll0_reg, pll1_reg, pll3_reg,
				pll_test_reg, original_pll0, startup_pll1,
				startup_pll3, &report->disabled_pll_ctrl0,
				&report->target_ready_dropped) != 0) {
			status = CARBOX_PLL_ISOLATED_PROBE_NOT_READY;
			goto restore_from_ana;
		}
		if ((CONFIG_SYS_PLL_ISOLATED_TARGET_HZ !=
		     CARBOX_SYS_PLL_NOMINAL_HZ) &&
		    CONFIG_SYS_PLL_ISOLATED_SDM_POWER) {
			uint32_t request_pll1 = (startup_pll1 &
				~CARBOX_PLL_FREQ_SEL_MASK) |
				((CONFIG_SYS_PLL_ISOLATED_FREQ_SEL & 0xFU) <<
				 CARBOX_PLL_FREQ_SEL_SHIFT);

			report->sdm_power_after_enable =
				((*pll1_reg & CARBOX_PLL_SDM_FCODE_POWER) != 0U) ?
				1U : 0U;
			if (CONFIG_SYS_PLL_ISOLATED_MANUAL_MODE) {
				/*
				 * The homologous Realtek PLL block uses hidden CTRL0
				 * bit2 as auto(1)/manual(0).  Force the transition
				 * before publishing the raw SDM request.
				 */
				*pll0_reg = *pll0_reg | CARBOX_PLL_AUTO_MODE;
				carbox_clock_barrier();
				*pll0_reg = *pll0_reg & ~CARBOX_PLL_AUTO_MODE;
				carbox_clock_barrier();
				report->manual_mode_transition = 1U;
			}
			carbox_pll_request(pll1_reg, pll3_reg, request_pll1,
				startup_pll3, &target);
		}
	} else {
		carbox_pll_request(pll1_reg, pll3_reg, original_pll1,
			original_pll3, &target);
	}
	report->target_pll_ctrl0 = *pll0_reg;
	report->target_pll_ctrl1 = *pll1_reg;
	report->target_pll_ctrl3 = *pll3_reg;
	report->target_pll_test = *pll_test_reg;
	report->target_ready =
		(report->target_pll_test & CARBOX_PLL_READY) != 0U ? 1U : 0U;
	if (report->target_ready == 0U) {
		status = CARBOX_PLL_ISOLATED_PROBE_NOT_READY;
		goto restore_from_ana;
	}

	/*
	 * Normal probes observe PLL_SYS through /2.  Do not use the ROM's
	 * SYS_CLK_300M policy for the direct probe: with the 400 MHz preset it
	 * rewrites CTRL1 from the boot-qualified 0x300 (/1) to 0x301 and produces
	 * about 266.7 MHz.  Re-publish the exact boot /1 mux image while executing
	 * from SRAM with IRQs masked.  This direct window ends before PLL restore.
	 */
	if (CONFIG_SYS_PLL_ISOLATED_DIRECT) {
		*clk_reg = original_clk;
	} else {
		hal_syson_set_sys_clk(SYS_CLK_150M);
	}
	carbox_clock_barrier();
	report->target_div2.clk_ctrl1 = *clk_reg;
	carbox_clock_measure_raw(&report->target_div2.cycles,
				 &report->target_div2.reference_us);

	/* Never modify or restore PLL_SYS while the CPU is sourced from it. */
	hal_syson_set_sys_clk(SYS_CLK_4M);
	carbox_clock_barrier();
	status = CARBOX_PLL_ISOLATED_PROBE_TARGET_VERIFY_FAILED;

restore_from_ana:
	if (CONFIG_SYS_PLL_ISOLATED_POWER_CYCLE) {
		(void)carbox_pll_power_cycle_image(pll0_reg, pll1_reg, pll3_reg,
			pll_test_reg, original_pll0, original_pll1, original_pll3,
			0, &report->restore_ready_dropped);
	} else {
		carbox_pll_restore_image(pll1_reg, pll3_reg,
				 original_pll1, original_pll3);
	}
	report->restored_pll_ctrl0 = *pll0_reg;
	if ((*pll_test_reg & CARBOX_PLL_READY) == 0U) {
		status = CARBOX_PLL_ISOLATED_PROBE_RESTORE_FAILED;
	}

restore_clock_only:
	hal_syson_set_sys_clk(SYS_CLK_300M);
	carbox_clock_barrier();
	if (*clk_reg != original_clk) {
		*clk_reg = original_clk;
		carbox_clock_barrier();
	}
	SystemCoreClock = CARBOX_SYS_PLL_NOMINAL_HZ;
	report->restored_sample.clk_ctrl1 = *clk_reg;
	carbox_clock_measure_raw(&report->restored_sample.cycles,
				 &report->restored_sample.reference_us);
	carbox_irq_restore(primask);

	/* 64-bit division is deliberately deferred until known-good 300 MHz/XIP. */
	report->before.measured_hz = carbox_clock_calculate_hz(
		report->before.cycles, report->before.reference_us);
	report->ana.measured_hz = carbox_clock_calculate_hz(
		report->ana.cycles, report->ana.reference_us);
	report->baseline_div2.measured_hz = carbox_clock_calculate_hz(
		report->baseline_div2.cycles,
		report->baseline_div2.reference_us);
	report->target_div2.measured_hz = carbox_clock_calculate_hz(
		report->target_div2.cycles, report->target_div2.reference_us);
	report->restored_sample.measured_hz = carbox_clock_calculate_hz(
		report->restored_sample.cycles,
		report->restored_sample.reference_us);
	report->restored =
		(report->restored_sample.clk_ctrl1 == original_clk &&
		 carbox_clock_measurement_matches(
			report->restored_sample.measured_hz,
			CARBOX_SYS_PLL_NOMINAL_HZ)) ? 1U : 0U;
	if (report->restored == 0U) {
		status = CARBOX_PLL_ISOLATED_PROBE_RESTORE_FAILED;
	} else if (status == CARBOX_PLL_ISOLATED_PROBE_TARGET_VERIFY_FAILED) {
		if (carbox_clock_measurement_matches(
			report->target_div2.measured_hz,
			report->expected_div2_hz)) {
			status = CARBOX_PLL_ISOLATED_PROBE_PASSED;
		} else if (carbox_clock_measurement_matches(
				report->target_div2.measured_hz, SYS_CLK_150M)) {
			status = CARBOX_PLL_ISOLATED_PROBE_PRESET_IGNORED;
		}
	}
	report->status = status;
	return status;
#endif
}

int carbox_system_overclock_early(void)
{
#if CONFIG_SYS_PLL_ISOLATED_PROBE
	return carbox_pll_isolated_probe_early();
#elif !CONFIG_SYS_PLL_OVERCLOCK
	return CARBOX_OVERCLOCK_DISABLED;
#else
	volatile uint32_t *clk_reg = carbox_syson_reg(CARBOX_SYS_CLK_CTRL1);
	volatile uint32_t *pll0_reg = carbox_syson_reg(CARBOX_PLL_SYS_CTRL0);
	volatile uint32_t *pll1_reg = carbox_syson_reg(CARBOX_PLL_SYS_CTRL1);
	volatile uint32_t *pll3_reg = carbox_syson_reg(CARBOX_PLL_SYS_CTRL3);
	volatile uint32_t *pll_test_reg = carbox_syson_reg(CARBOX_PLL_TEST);
	uint32_t original_clk = *clk_reg;
	uint32_t original_pll0 = *pll0_reg;
	uint32_t original_pll1 = *pll1_reg;
	uint32_t original_pll3 = *pll3_reg;
	struct carbox_pll_codes target;
	uint32_t startup_pll1;
	uint32_t request_pll1;
	uint8_t ready_dropped;
	uint32_t primask;
	uint32_t measured_hz;

	carbox_overclock_report.target_hz = CONFIG_SYS_PLL_TARGET_HZ;
	carbox_overclock_report.pll_target_hz = 0U;
	carbox_overclock_report.measured_hz = 0U;
	carbox_overclock_report.rollback_measured_hz = 0U;
	carbox_overclock_report.cycles = 0U;
	carbox_overclock_report.reference_us = 0U;
	carbox_overclock_report.divn = 0U;
	carbox_overclock_report.fon = 0U;
	carbox_overclock_report.fof = 0U;
	carbox_overclock_report.tolerance_pct =
		CARBOX_CLOCK_VERIFY_TOLERANCE_PCT;
	carbox_overclock_report.measurement_valid = 0U;
	carbox_overclock_report.rolled_back = 0U;

#if CONFIG_SYS_PLL_TARGET_HZ != 400000000UL
#error "Milestone 2 is qualified only for PLL_SYS/CPU 400 MHz"
#endif

	/*
	 * Only modify the board's measured 300 MHz, 40 MHz-XTAL ROM boot
	 * configuration.  Its clock path is 0x300: PLL selected, divider selector
	 * zero (/1), and the 300 MHz PLL output selected.  Bit 9 enables the
	 * programmable divider block; it does not itself mean divide-by-two.
	 */
	if ((SystemCoreClock != CARBOX_SYS_PLL_NOMINAL_HZ) ||
	    ((original_clk & (CARBOX_SYS_CLK_SOURCE_PLL |
			      CARBOX_SYS_CLK_DIV_ENABLE |
			      CARBOX_SYS_CLK_PLL_DIV_MASK | 1UL)) !=
	     (CARBOX_SYS_CLK_SOURCE_PLL | CARBOX_SYS_CLK_DIV_ENABLE |
	      CARBOX_SYS_CLK_PLL_300M))) {
		return CARBOX_OVERCLOCK_BAD_BOOT_CLOCK;
	}
	if (((original_pll0 & (CARBOX_PLL_ENABLE | CARBOX_PLL_CLK_ENABLE)) !=
	     (CARBOX_PLL_ENABLE | CARBOX_PLL_CLK_ENABLE)) ||
	    ((original_pll1 & CARBOX_PLL_XTAL_MASK) != 0U) ||
	    ((*pll_test_reg & CARBOX_PLL_READY) == 0U)) {
		return CARBOX_OVERCLOCK_BAD_PLL_STATE;
	}

	if (carbox_pll_encode_cpu_hz(CONFIG_SYS_PLL_TARGET_HZ, &target) != 0) {
		return CARBOX_OVERCLOCK_BAD_PLL_STATE;
	}
	carbox_overclock_report.pll_target_hz = target.pll_hz;
	carbox_overclock_report.divn = (uint8_t)target.divn;
	carbox_overclock_report.fon = (uint8_t)target.fon;
	carbox_overclock_report.fof = (uint16_t)target.fof;
	/* The experimentally qualified 400 MHz preset reports this SDM image. */
	if ((target.pll_hz != 400000000UL) || (target.divn != 8U) ||
	    (target.fon != 0U) || (target.fof != 0U)) {
		return CARBOX_OVERCLOCK_BAD_PLL_STATE;
	}

	primask = carbox_irq_save();

	/*
	 * Snapshot the qualified 300 MHz SPIC calibration and select a conservative
	 * divider before raising PLL_SYS.  Final delay/dummy calibration runs only
	 * after the 400 MHz clock has passed the independent timer measurement.
	 */
	if (carbox_spic_overclock_prepare(CONFIG_SYS_PLL_TARGET_HZ) < 0) {
		carbox_irq_restore(primask);
		return CARBOX_OVERCLOCK_SPIC_PREPARE_FAILED;
	}

	/* Leave PLL_SYS before selecting and power-cycling the proven 400 MHz preset. */
	hal_syson_set_sys_clk(SYS_CLK_4M);
	carbox_clock_barrier();
	if ((*clk_reg & CARBOX_SYS_CLK_SOURCE_PLL) != 0U) {
		carbox_spic_overclock_restore();
		carbox_irq_restore(primask);
		return CARBOX_OVERCLOCK_BAD_BOOT_CLOCK;
	}

	/*
	 * Reproduce the isolated probe sequence that measured 400.394 MHz:
	 * restart the PLL with SDM F-code power enabled, force the documented
	 * auto-to-manual transition, then select coarse preset zero.
	 */
	startup_pll1 = original_pll1 | CARBOX_PLL_SDM_FCODE_POWER;
	if (carbox_pll_power_cycle_image(pll0_reg, pll1_reg, pll3_reg,
			pll_test_reg, original_pll0, startup_pll1,
			original_pll3, 0, &ready_dropped) != 0) {
		(void)carbox_overclock_restore_300_from_ana(clk_reg, pll0_reg,
			pll1_reg, pll3_reg, pll_test_reg, original_clk,
			original_pll0, original_pll1, original_pll3);
		carbox_irq_restore(primask);
		return CARBOX_OVERCLOCK_LOCK_TIMEOUT;
	}
	*pll0_reg = *pll0_reg | CARBOX_PLL_AUTO_MODE;
	carbox_clock_barrier();
	*pll0_reg = *pll0_reg & ~CARBOX_PLL_AUTO_MODE;
	carbox_clock_barrier();
	request_pll1 = startup_pll1 & ~CARBOX_PLL_FREQ_SEL_MASK;
	carbox_pll_request(pll1_reg, pll3_reg, request_pll1,
			   original_pll3, &target);
	if ((*pll_test_reg & CARBOX_PLL_READY) == 0U) {
		(void)carbox_overclock_restore_300_from_ana(clk_reg, pll0_reg,
			pll1_reg, pll3_reg, pll_test_reg, original_clk,
			original_pll0, original_pll1, original_pll3);
		carbox_irq_restore(primask);
		return CARBOX_OVERCLOCK_LOCK_TIMEOUT;
	}

	/* Exact boot mux image 0x300 is the proven PLL_SYS /1 path. */
	*clk_reg = original_clk;
	SystemCoreClock = CONFIG_SYS_PLL_TARGET_HZ;
	carbox_clock_barrier();

	/*
	 * Do not perform the 64-bit Hz calculation yet: libgcc's
	 * __aeabi_uldivmod resides in XIP on this image.  Complete SPIC training
	 * using only the verified SRAM/ROM call graph before the first possible
	 * external-flash instruction fetch at 400 MHz.
	 */
	if (carbox_spic_overclock_calibrate(CONFIG_SYS_PLL_TARGET_HZ) < 0) {
		int restore_status;

		hal_syson_set_sys_clk(SYS_CLK_4M);
		carbox_clock_barrier();
		restore_status = carbox_overclock_restore_300_from_ana(clk_reg,
			pll0_reg, pll1_reg, pll3_reg, pll_test_reg, original_clk,
			original_pll0, original_pll1, original_pll3);
		carbox_irq_restore(primask);
		if (restore_status != 0) {
			return CARBOX_OVERCLOCK_RESTORE_FAILED;
		}
		return CARBOX_OVERCLOCK_SPIC_CALIBRATION_FAILED;
	}

	measured_hz = carbox_clock_measure_hz();
	carbox_overclock_report.measured_hz = measured_hz;
	carbox_overclock_report.measurement_valid = measured_hz != 0U ? 1U : 0U;
	if (!carbox_clock_measurement_matches(measured_hz,
					 CONFIG_SYS_PLL_TARGET_HZ)) {
		int restore_status;

		hal_syson_set_sys_clk(SYS_CLK_4M);
		carbox_clock_barrier();
		restore_status = carbox_overclock_restore_300_from_ana(clk_reg,
			pll0_reg, pll1_reg, pll3_reg, pll_test_reg, original_clk,
			original_pll0, original_pll1, original_pll3);
		carbox_irq_restore(primask);
		if (restore_status != 0) {
			return CARBOX_OVERCLOCK_RESTORE_FAILED;
		}
		return CARBOX_OVERCLOCK_CLOCK_VERIFY_FAILED;
	}
	/* The clock mux/divider must remain byte-for-byte identical to ROM state. */
	if (*clk_reg != original_clk) {
		int restore_status;

		hal_syson_set_sys_clk(SYS_CLK_4M);
		carbox_clock_barrier();
		restore_status = carbox_overclock_restore_300_from_ana(clk_reg,
			pll0_reg, pll1_reg, pll3_reg, pll_test_reg, original_clk,
			original_pll0, original_pll1, original_pll3);
		carbox_irq_restore(primask);
		return restore_status == 0 ? CARBOX_OVERCLOCK_BAD_BOOT_CLOCK :
			CARBOX_OVERCLOCK_RESTORE_FAILED;
	}
	carbox_irq_restore(primask);

	return CARBOX_OVERCLOCK_APPLIED;
#endif
}

int carbox_sysclk_probe_early(void)
{
#if !CONFIG_SYS_CLK_SWITCH_PROBE
	carbox_sysclk_probe_report.status = CARBOX_SYSCLK_PROBE_DISABLED;
	return CARBOX_SYSCLK_PROBE_DISABLED;
#else
	volatile uint32_t *clk_reg = carbox_syson_reg(CARBOX_SYS_CLK_CTRL1);
	struct carbox_sysclk_probe_sample *before =
		&carbox_sysclk_probe_report.before;
	struct carbox_sysclk_probe_sample *switched =
		&carbox_sysclk_probe_report.switched;
	struct carbox_sysclk_probe_sample *restored =
		&carbox_sysclk_probe_report.restored_sample;
	uint32_t original_clk = *clk_reg;
	uint32_t primask;
	int status;

	carbox_sysclk_probe_report.status = CARBOX_SYSCLK_PROBE_BAD_BOOT_CLOCK;
	carbox_sysclk_probe_report.restored = 0U;
	before->requested_hz = SYS_CLK_300M;
	before->measured_hz = 0U;
	before->clk_ctrl1 = original_clk;
	before->cycles = 0U;
	before->reference_us = 0U;
	switched->requested_hz = SYS_CLK_200M;
	switched->measured_hz = 0U;
	switched->clk_ctrl1 = original_clk;
	switched->cycles = 0U;
	switched->reference_us = 0U;
	restored->requested_hz = SYS_CLK_300M;
	restored->measured_hz = 0U;
	restored->clk_ctrl1 = original_clk;
	restored->cycles = 0U;
	restored->reference_us = 0U;

	/* Only characterize the exact, already-observed 300 MHz ROM boot path. */
	if ((SystemCoreClock != CARBOX_SYS_PLL_NOMINAL_HZ) ||
	    ((original_clk & (CARBOX_SYS_CLK_SOURCE_PLL |
			      CARBOX_SYS_CLK_DIV_ENABLE |
			      CARBOX_SYS_CLK_PLL_DIV_MASK | 1UL)) !=
	     (CARBOX_SYS_CLK_SOURCE_PLL | CARBOX_SYS_CLK_DIV_ENABLE |
	      CARBOX_SYS_CLK_PLL_300M))) {
		return CARBOX_SYSCLK_PROBE_BAD_BOOT_CLOCK;
	}

	primask = carbox_irq_save();
	carbox_clock_measure_raw(&before->cycles, &before->reference_us);

	/* ROM owns the undocumented mux/divider encoding; do not guess its bits. */
	hal_syson_set_sys_clk(SYS_CLK_200M);
	carbox_clock_barrier();
	switched->clk_ctrl1 = *clk_reg;
	carbox_clock_measure_raw(&switched->cycles,
				 &switched->reference_us);

	hal_syson_set_sys_clk(SYS_CLK_300M);
	carbox_clock_barrier();
	/* Preserve the exact ROM-established boot image, not merely an equivalent. */
	if (*clk_reg != original_clk) {
		*clk_reg = original_clk;
		carbox_clock_barrier();
	}
	restored->clk_ctrl1 = *clk_reg;
	SystemCoreClock = CARBOX_SYS_PLL_NOMINAL_HZ;
	carbox_clock_measure_raw(&restored->cycles,
				 &restored->reference_us);
	carbox_irq_restore(primask);

	/* Perform 64-bit division only after the original 300 MHz path is back. */
	before->measured_hz = carbox_clock_calculate_hz(before->cycles,
						before->reference_us);
	switched->measured_hz = carbox_clock_calculate_hz(switched->cycles,
						  switched->reference_us);
	restored->measured_hz = carbox_clock_calculate_hz(restored->cycles,
						  restored->reference_us);
	carbox_sysclk_probe_report.restored =
		(restored->clk_ctrl1 == original_clk &&
		 carbox_clock_measurement_matches(restored->measured_hz,
						       SYS_CLK_300M)) ? 1U : 0U;

	if (carbox_sysclk_probe_report.restored == 0U) {
		status = CARBOX_SYSCLK_PROBE_RESTORE_FAILED;
	} else if (!carbox_clock_measurement_matches(before->measured_hz,
						      SYS_CLK_300M) ||
		   !carbox_clock_measurement_matches(switched->measured_hz,
						      SYS_CLK_200M)) {
		status = CARBOX_SYSCLK_PROBE_SWITCH_FAILED;
	} else {
		status = CARBOX_SYSCLK_PROBE_PASSED;
	}
	carbox_sysclk_probe_report.status = status;
	return status;
#endif
}

void carbox_sysclk_probe_get_report(struct carbox_sysclk_probe_report *report)
{
	if (report != 0) {
		*report = carbox_sysclk_probe_report;
	}
}

void carbox_pll_isolated_probe_get_report(
	struct carbox_pll_isolated_probe_report *report)
{
	if (report != 0) {
		*report = carbox_pll_isolated_report;
	}
}

void carbox_system_overclock_get_report(
	struct carbox_system_overclock_report *report)
{
	if (report != 0) {
		*report = carbox_overclock_report;
	}
}
