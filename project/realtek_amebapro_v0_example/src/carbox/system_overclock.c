#include <stdint.h>

#include "system_overclock.h"

#ifndef CONFIG_SYS_PLL_OVERCLOCK
#define CONFIG_SYS_PLL_OVERCLOCK 0
#endif

#ifndef CONFIG_SYS_PLL_TARGET_HZ
#define CONFIG_SYS_PLL_TARGET_HZ 360000000UL
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

#define CARBOX_PLL_CLK_ENABLE            (1UL << 8)
#define CARBOX_PLL_ENABLE                (1UL << 29)
#define CARBOX_PLL_READY                 (1UL << 26)

#define CARBOX_PLL_DIVN_SHIFT                    5U
#define CARBOX_PLL_DIVN_MASK             (0x3FUL << CARBOX_PLL_DIVN_SHIFT)
#define CARBOX_PLL_XTAL_SHIFT                   28U
#define CARBOX_PLL_XTAL_MASK             (0xFUL << CARBOX_PLL_XTAL_SHIFT)
#define CARBOX_PLL_FON_SHIFT                    16U
#define CARBOX_PLL_FON_MASK              (0x7UL << CARBOX_PLL_FON_SHIFT)
#define CARBOX_PLL_FOF_SHIFT                    19U
#define CARBOX_PLL_FOF_MASK              (0x1FFFUL << CARBOX_PLL_FOF_SHIFT)

#define CARBOX_PLL_FRAC_BITS                    16U
#define CARBOX_PLL_FON_TO_FIXED_SHIFT           13U
#define CARBOX_PLL_LOCK_LOOPS               200000U
#define CARBOX_PLL_SETTLE_LOOPS                2000U

extern uint32_t SystemCoreClock;

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

static int carbox_pll_wait_ready(void)
{
	uint32_t i;

	for (i = 0; i < CARBOX_PLL_LOCK_LOOPS; ++i) {
		if ((*carbox_syson_reg(CARBOX_PLL_TEST) & CARBOX_PLL_READY) != 0U) {
			return 0;
		}
	}
	return -1;
}

static int carbox_pll_wait_stopped(void)
{
	uint32_t i;

	for (i = 0; i < CARBOX_PLL_LOCK_LOOPS; ++i) {
		if ((*carbox_syson_reg(CARBOX_PLL_TEST) & CARBOX_PLL_READY) == 0U) {
			return 0;
		}
	}
	return -1;
}

static uint32_t carbox_pll_scaled_ctrl3(uint32_t ctrl1, uint32_t ctrl3)
{
	uint64_t divider_fixed;
	uint32_t divn;
	uint32_t fon;
	uint32_t fof;

	divn = (ctrl1 & CARBOX_PLL_DIVN_MASK) >> CARBOX_PLL_DIVN_SHIFT;
	fon = (ctrl3 & CARBOX_PLL_FON_MASK) >> CARBOX_PLL_FON_SHIFT;
	fof = (ctrl3 & CARBOX_PLL_FOF_MASK) >> CARBOX_PLL_FOF_SHIFT;

	/*
	 * Realtek's SDM feedback divider is DIVN+2, followed by a 3-bit
	 * one-eighth term and a 13-bit fractional term.  Scaling the complete
	 * fixed-point divider preserves the boot ROM's board/XTAL calibration and
	 * also works whether the PLL output subsequently uses /1 or /2.
	 */
	divider_fixed = ((uint64_t)(divn + CARBOX_SYS_PLL_DIVN_BIAS) <<
			 CARBOX_PLL_FRAC_BITS) |
			 ((uint64_t)fon << CARBOX_PLL_FON_TO_FIXED_SHIFT) |
			 fof;
	divider_fixed = (divider_fixed * CONFIG_SYS_PLL_TARGET_HZ +
			 (CARBOX_SYS_PLL_NOMINAL_HZ / 2U)) /
			 CARBOX_SYS_PLL_NOMINAL_HZ;

	divn = (uint32_t)(divider_fixed >> CARBOX_PLL_FRAC_BITS);
	if (divn < CARBOX_SYS_PLL_DIVN_BIAS) {
		return ctrl3;
	}
	divn -= CARBOX_SYS_PLL_DIVN_BIAS;
	fon = (uint32_t)((divider_fixed >> CARBOX_PLL_FON_TO_FIXED_SHIFT) & 0x7U);
	fof = (uint32_t)(divider_fixed & 0x1FFFU);

	/* DIVN is in CTRL1; CTRL3 contains only the two fractional fields. */
	ctrl3 &= ~(CARBOX_PLL_FON_MASK | CARBOX_PLL_FOF_MASK);
	ctrl3 |= (fon << CARBOX_PLL_FON_SHIFT) | (fof << CARBOX_PLL_FOF_SHIFT);
	return ctrl3;
}

static uint32_t carbox_pll_scaled_ctrl1(uint32_t ctrl1)
{
	uint32_t ctrl3 = *carbox_syson_reg(CARBOX_PLL_SYS_CTRL3);
	uint64_t divider_fixed;
	uint32_t divn;

	divn = (ctrl1 & CARBOX_PLL_DIVN_MASK) >> CARBOX_PLL_DIVN_SHIFT;
	divider_fixed = ((uint64_t)(divn + CARBOX_SYS_PLL_DIVN_BIAS) <<
			 CARBOX_PLL_FRAC_BITS) |
			 ((uint64_t)((ctrl3 & CARBOX_PLL_FON_MASK) >> CARBOX_PLL_FON_SHIFT) <<
			  CARBOX_PLL_FON_TO_FIXED_SHIFT) |
			 ((ctrl3 & CARBOX_PLL_FOF_MASK) >> CARBOX_PLL_FOF_SHIFT);
	divider_fixed = (divider_fixed * CONFIG_SYS_PLL_TARGET_HZ +
			 (CARBOX_SYS_PLL_NOMINAL_HZ / 2U)) /
			 CARBOX_SYS_PLL_NOMINAL_HZ;

	divn = (uint32_t)(divider_fixed >> CARBOX_PLL_FRAC_BITS);
	if ((divn < CARBOX_SYS_PLL_DIVN_BIAS) ||
	    ((divn - CARBOX_SYS_PLL_DIVN_BIAS) > 0x3FU)) {
		return ctrl1;
	}

	ctrl1 &= ~CARBOX_PLL_DIVN_MASK;
	ctrl1 |= (divn - CARBOX_SYS_PLL_DIVN_BIAS) << CARBOX_PLL_DIVN_SHIFT;
	return ctrl1;
}

int carbox_system_overclock_early(void)
{
#if !CONFIG_SYS_PLL_OVERCLOCK
	return CARBOX_OVERCLOCK_DISABLED;
#else
	volatile uint32_t *clk_reg = carbox_syson_reg(CARBOX_SYS_CLK_CTRL1);
	volatile uint32_t *pll0_reg = carbox_syson_reg(CARBOX_PLL_SYS_CTRL0);
	volatile uint32_t *pll1_reg = carbox_syson_reg(CARBOX_PLL_SYS_CTRL1);
	volatile uint32_t *pll3_reg = carbox_syson_reg(CARBOX_PLL_SYS_CTRL3);
	uint32_t original_clk = *clk_reg;
	uint32_t original_pll0 = *pll0_reg;
	uint32_t original_pll1 = *pll1_reg;
	uint32_t original_pll3 = *pll3_reg;
	uint32_t new_pll1;
	uint32_t new_pll3;
	uint32_t primask;

#if (CONFIG_SYS_PLL_TARGET_HZ <= CARBOX_SYS_PLL_NOMINAL_HZ) || \
    (CONFIG_SYS_PLL_TARGET_HZ > 360000000UL)
#error "CONFIG_SYS_PLL_TARGET_HZ must be in the experimental 300-360 MHz range"
#endif

	/* Only modify the known-good 300 MHz, 40 MHz-XTAL boot configuration. */
	if ((original_clk & (CARBOX_SYS_CLK_SOURCE_PLL | CARBOX_SYS_CLK_DIV_ENABLE | 1UL)) !=
	    (CARBOX_SYS_CLK_SOURCE_PLL | CARBOX_SYS_CLK_PLL_300M)) {
		return CARBOX_OVERCLOCK_BAD_BOOT_CLOCK;
	}
	if (((original_pll0 & (CARBOX_PLL_ENABLE | CARBOX_PLL_CLK_ENABLE)) !=
	     (CARBOX_PLL_ENABLE | CARBOX_PLL_CLK_ENABLE)) ||
	    ((original_pll1 & CARBOX_PLL_XTAL_MASK) != 0U) ||
	    ((*carbox_syson_reg(CARBOX_PLL_TEST) & CARBOX_PLL_READY) == 0U)) {
		return CARBOX_OVERCLOCK_BAD_PLL_STATE;
	}

	new_pll1 = carbox_pll_scaled_ctrl1(original_pll1);
	new_pll3 = carbox_pll_scaled_ctrl3(original_pll1, original_pll3);
	if ((new_pll1 == original_pll1) && (new_pll3 == original_pll3)) {
		return CARBOX_OVERCLOCK_BAD_PLL_STATE;
	}

	primask = carbox_irq_save();

	/* Run temporarily from the 4 MHz ANA clock while SYS PLL is relocked. */
	*clk_reg = original_clk & ~CARBOX_SYS_CLK_SOURCE_PLL;
	carbox_clock_barrier();

	*pll0_reg = original_pll0 & ~(CARBOX_PLL_CLK_ENABLE | CARBOX_PLL_ENABLE);
	carbox_clock_barrier();
	carbox_pll_settle_delay();
	if (carbox_pll_wait_stopped() != 0) {
		*pll0_reg = original_pll0;
		*clk_reg = original_clk;
		SystemCoreClock = CARBOX_SYS_PLL_NOMINAL_HZ;
		carbox_clock_barrier();
		carbox_irq_restore(primask);
		return CARBOX_OVERCLOCK_STOP_TIMEOUT;
	}

	*pll1_reg = new_pll1;
	*pll3_reg = new_pll3;
	carbox_clock_barrier();

	*pll0_reg = original_pll0 & ~CARBOX_PLL_CLK_ENABLE;
	carbox_clock_barrier();
	if (carbox_pll_wait_ready() != 0) {
		/* Restore the exact ROM-established PLL state before returning. */
		*pll0_reg = original_pll0 & ~(CARBOX_PLL_CLK_ENABLE | CARBOX_PLL_ENABLE);
		carbox_clock_barrier();
		(void)carbox_pll_wait_stopped();
		*pll1_reg = original_pll1;
		*pll3_reg = original_pll3;
		carbox_clock_barrier();
		*pll0_reg = original_pll0 & ~CARBOX_PLL_CLK_ENABLE;
		carbox_clock_barrier();
		if (carbox_pll_wait_ready() != 0) {
			SystemCoreClock = CARBOX_SYS_PLL_INPUT_HZ / 10U;
			carbox_irq_restore(primask);
			return CARBOX_OVERCLOCK_RESTORE_FAILED;
		}
		*pll0_reg = original_pll0;
		*clk_reg = original_clk;
		SystemCoreClock = CARBOX_SYS_PLL_NOMINAL_HZ;
		carbox_clock_barrier();
		carbox_irq_restore(primask);
		return CARBOX_OVERCLOCK_LOCK_TIMEOUT;
	}

	*pll0_reg = original_pll0;
	carbox_clock_barrier();
	*clk_reg = original_clk;
	SystemCoreClock = CONFIG_SYS_PLL_TARGET_HZ;
	carbox_clock_barrier();
	carbox_irq_restore(primask);

	return CARBOX_OVERCLOCK_APPLIED;
#endif
}
