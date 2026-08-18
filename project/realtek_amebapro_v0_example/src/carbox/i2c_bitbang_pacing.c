#include "objects.h"
#include "gpio_api.h"
#include "cmsis.h"
#include "diag.h"

#include "i2c_bitbang_pacing.h"

#ifndef CONFIG_I2C_BITBANG_PACING
#define CONFIG_I2C_BITBANG_PACING 0
#endif

#ifndef CONFIG_I2C_BITBANG_BASELINE_HZ
#define CONFIG_I2C_BITBANG_BASELINE_HZ 300000000UL
#endif

#ifndef CONFIG_SYS_PLL_TARGET_HZ
#define CONFIG_SYS_PLL_TARGET_HZ CONFIG_I2C_BITBANG_BASELINE_HZ
#endif

/* lib_SystemLib.a(I2C.o) initializes GPIO 111 as its software-I2C SCL. */
#define CARBOX_I2C_BITBANG_SCL_PIN       111U
#define CARBOX_I2C_BITBANG_SDA_PIN       110U
#define CARBOX_I2C_ACTIVE_GAP_MAX_US     100U

typedef struct carbox_i2c_pacing_stats_s {
	volatile uint32_t scl_calls;
	volatile uint32_t edges;
	volatile uint32_t delayed_edges;
	volatile uint32_t idle_resets;
	volatile uint32_t dwt_unavailable;
	volatile uint32_t delay_cycles;
	volatile uint32_t delay_cycles_max;
	volatile uint32_t interval_cycles_max;
	volatile uint32_t scl_registrations;
	volatile uint32_t sda_registrations;
} carbox_i2c_pacing_stats_t;

static carbox_i2c_pacing_stats_t pacing_stats;
static uint32_t pacing_last_edge_cycle;
static int pacing_last_value;
static uint8_t pacing_have_edge;
static gpio_t *volatile pacing_scl_object;
static gpio_t *volatile pacing_sda_object;

void __real_gpio_init(gpio_t *obj, PinName pin);
void __real_gpio_write(gpio_t *obj, int value);

void __wrap_gpio_init(gpio_t *obj, PinName pin)
{
#if CONFIG_I2C_BITBANG_PACING
	/*
	 * Register by the pre-HAL PinName rather than adapter.pin_name: the ROM
	 * GPIO initializer is allowed to translate the latter to an IP pin ID.
	 */
	if ((uint32_t)pin == CARBOX_I2C_BITBANG_SCL_PIN) {
		pacing_scl_object = obj;
		pacing_stats.scl_registrations++;
		pacing_have_edge = 0U;
	} else if ((uint32_t)pin == CARBOX_I2C_BITBANG_SDA_PIN) {
		pacing_sda_object = obj;
		pacing_stats.sda_registrations++;
	}
#endif
	__real_gpio_init(obj, pin);
}

static inline uint32_t carbox_i2c_pacing_extra_cycles(uint32_t elapsed)
{
#if CONFIG_SYS_PLL_TARGET_HZ > CONFIG_I2C_BITBANG_BASELINE_HZ
	return (uint32_t)(((uint64_t)elapsed *
		(CONFIG_SYS_PLL_TARGET_HZ - CONFIG_I2C_BITBANG_BASELINE_HZ)) /
		CONFIG_I2C_BITBANG_BASELINE_HZ);
#else
	(void)elapsed;
	return 0U;
#endif
}

void __wrap_gpio_write(gpio_t *obj, int value)
{
#if CONFIG_I2C_BITBANG_PACING
	uint32_t now;
	uint32_t elapsed;
	uint32_t extra;
	uint32_t max_gap_cycles;

	if (obj != NULL && obj == pacing_scl_object) {
		pacing_stats.scl_calls++;
		value = value != 0;

		if (pacing_have_edge != 0U && value != pacing_last_value) {
			pacing_stats.edges++;
			if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0U) {
				pacing_stats.dwt_unavailable++;
			} else {
				now = DWT->CYCCNT;
				elapsed = now - pacing_last_edge_cycle;
				max_gap_cycles = CONFIG_SYS_PLL_TARGET_HZ /
					(1000000UL / CARBOX_I2C_ACTIVE_GAP_MAX_US);
				if (elapsed <= max_gap_cycles) {
					extra = carbox_i2c_pacing_extra_cycles(elapsed);
					if (elapsed > pacing_stats.interval_cycles_max)
						pacing_stats.interval_cycles_max = elapsed;
					if (extra != 0U) {
						uint32_t start = DWT->CYCCNT;

						while ((uint32_t)(DWT->CYCCNT - start) < extra)
							__NOP();
						pacing_stats.delayed_edges++;
						pacing_stats.delay_cycles += extra;
						if (extra > pacing_stats.delay_cycles_max)
							pacing_stats.delay_cycles_max = extra;
					}
				} else {
					pacing_stats.idle_resets++;
				}
			}
		}

		__real_gpio_write(obj, value);
		pacing_last_value = value;
		pacing_have_edge = 1U;
		if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) != 0U)
			pacing_last_edge_cycle = DWT->CYCCNT;
		return;
	}
#endif

	__real_gpio_write(obj, value);
}

void carbox_i2c_bitbang_pacing_report(uint32_t sequence)
{
#if CONFIG_I2C_BITBANG_PACING
	uint32_t primask = __get_PRIMASK();
	carbox_i2c_pacing_stats_t snapshot;
	gpio_t *scl_object;
	gpio_t *sda_object;

	__disable_irq();
	snapshot = pacing_stats;
	scl_object = pacing_scl_object;
	sda_object = pacing_sda_object;
	pacing_stats.scl_calls = 0U;
	pacing_stats.edges = 0U;
	pacing_stats.delayed_edges = 0U;
	pacing_stats.idle_resets = 0U;
	pacing_stats.dwt_unavailable = 0U;
	pacing_stats.delay_cycles = 0U;
	pacing_stats.delay_cycles_max = 0U;
	pacing_stats.interval_cycles_max = 0U;
	/* Registration counters are lifetime state and intentionally retained. */
	pacing_stats.scl_registrations = snapshot.scl_registrations;
	pacing_stats.sda_registrations = snapshot.sda_registrations;
	if (primask == 0U)
		__enable_irq();

	rt_printf("[I2CPACE][%lu] scl_calls/edges/delayed=%lu/%lu/%lu "
		  "idle_reset/dwt_off=%lu/%lu delay_cycles total/max=%lu/%lu "
		  "interval_max=%lu register scl/sda=%lu/%lu obj=%p/%p "
		  "pins=%u/%u baseline/target=%lu/%luHz\r\n",
		  (unsigned long)sequence,
		  (unsigned long)snapshot.scl_calls,
		  (unsigned long)snapshot.edges,
		  (unsigned long)snapshot.delayed_edges,
		  (unsigned long)snapshot.idle_resets,
		  (unsigned long)snapshot.dwt_unavailable,
		  (unsigned long)snapshot.delay_cycles,
		  (unsigned long)snapshot.delay_cycles_max,
		  (unsigned long)snapshot.interval_cycles_max,
		  (unsigned long)snapshot.scl_registrations,
		  (unsigned long)snapshot.sda_registrations,
		  (void *)scl_object, (void *)sda_object,
		  (unsigned int)CARBOX_I2C_BITBANG_SCL_PIN,
		  (unsigned int)CARBOX_I2C_BITBANG_SDA_PIN,
		  (unsigned long)CONFIG_I2C_BITBANG_BASELINE_HZ,
		  (unsigned long)CONFIG_SYS_PLL_TARGET_HZ);
#else
	(void)sequence;
#endif
}
