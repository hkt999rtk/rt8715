#include <stdint.h>

#include "cmsis.h"
#include "hal_spic.h"
#include "hal_spic_nsc.h"
#include "rtl8195bhp_spic_type.h"

#include "spic_overclock.h"

#ifndef CONFIG_SPIC_ADAPTIVE_OVERCLOCK
#define CONFIG_SPIC_ADAPTIVE_OVERCLOCK 0
#endif

#ifndef CONFIG_SPIC_QUALIFIED_MAX_HZ
#define CONFIG_SPIC_QUALIFIED_MAX_HZ 75000000UL
#endif

#ifndef CONFIG_SPIC_CAL_MIN_WINDOW
#define CONFIG_SPIC_CAL_MIN_WINDOW 8U
#endif

#ifndef CONFIG_SPIC_CAL_VERIFY_COUNT
#define CONFIG_SPIC_CAL_VERIFY_COUNT 4U
#endif

#if (CONFIG_SPIC_QUALIFIED_MAX_HZ == 0)
#error "CONFIG_SPIC_QUALIFIED_MAX_HZ must be non-zero"
#endif

#if (CONFIG_SPIC_CAL_MIN_WINDOW == 0) || (CONFIG_SPIC_CAL_MIN_WINDOW > 100)
#error "CONFIG_SPIC_CAL_MIN_WINDOW must be in the range 1..100"
#endif

#if (CONFIG_SPIC_CAL_VERIFY_COUNT == 0)
#error "CONFIG_SPIC_CAL_VERIFY_COUNT must be non-zero"
#endif

#define CARBOX_SPIC_DTR_MAX_HZ 50000000UL
#define CARBOX_SPIC_MAX_DELAY  99U
#define CARBOX_SPIC_MAX_DIV    MAX_BAUD_RATE

extern hal_spic_adaptor_t *pglob_spic_adaptor;
extern hal_spic_seq_setting_t seq_setting[10][CPU_CLK_TYPE_NO];

struct carbox_spic_snapshot {
	SPIC_Type *dev;
	spic_init_para_t slot;
	uint32_t baudr;
	uint32_t fbaudr;
	uint32_t auto_length;
	uint32_t auto_length_seq;
	uint32_t valid_cmd;
	uint8_t cpu_type;
	uint8_t io_mode;
	uint8_t seq_dum_len;
	uint8_t active;
};

static struct carbox_spic_snapshot carbox_spic_snapshot;
static struct carbox_spic_overclock_report carbox_spic_report;

static int carbox_spic_is_dtr(uint8_t mode)
{
	return (mode == SpicQuadIODtrMode) ||
	       (mode == SpicQpiDtrMode) ||
	       (mode == SpicOpiDtrMode);
}

static uint32_t carbox_spic_qualified_hz(uint8_t mode)
{
	uint32_t limit = CONFIG_SPIC_QUALIFIED_MAX_HZ;

	if (carbox_spic_is_dtr(mode) && (limit > CARBOX_SPIC_DTR_MAX_HZ)) {
		limit = CARBOX_SPIC_DTR_MAX_HZ;
	}
	return limit;
}

static uint8_t carbox_spic_safe_divider(uint32_t sys_hz, uint32_t max_spic_hz,
					uint8_t mode)
{
	uint32_t denominator;
	uint32_t divider;

	if ((sys_hz == 0U) || (max_spic_hz == 0U) ||
	    (max_spic_hz > (UINT32_MAX / 2U))) {
		return 0U;
	}

	denominator = max_spic_hz * 2U;
	divider = sys_hz / denominator;
	if ((sys_hz % denominator) != 0U) {
		++divider;
	}
	if (divider < MIN_BAUD_RATE) {
		divider = MIN_BAUD_RATE;
	}
	/* The slow 0x03 one-I/O command needs the SDK's conservative divider. */
	if ((mode == SpicOneIOMode) && (divider < 5U)) {
		divider = 5U;
	}
	if (divider > CARBOX_SPIC_MAX_DIV) {
		return 0U;
	}
	return (uint8_t)divider;
}

static uint8_t carbox_spic_default_dummy(hal_spic_adaptor_t *adaptor)
{
	return *(((volatile uint8_t *)adaptor->dummy_cycle) + adaptor->spic_bit_mode);
}

static void carbox_spic_apply(SPIC_Type *dev, uint8_t divider,
			      uint8_t dummy, uint8_t delay)
{
	spic_disable_rtl8195bhp(dev);
	spic_set_baudr_rtl8195bhp(dev, divider);
	spic_set_fbaudr_rtl8195bhp(dev, divider);
	spic_set_dummy_cycle_rtl8195bhp(dev, dummy);
	spic_set_delay_line(delay);
	spic_enable_rtl8195bhp(dev);
}

static uint8_t carbox_spic_find_window(SPIC_Type *dev, uint8_t divider,
				       uint8_t default_dummy,
				       uint8_t *best_dummy,
				       uint8_t *best_delay)
{
	uint32_t auto_len;
	uint32_t auto_start = (uint32_t)default_dummy * divider * 2U + 2U;
	uint32_t auto_end = (uint32_t)default_dummy * divider * 2U + MAX_AUTO_LENGTH;
	uint8_t best_width = 0U;
	uint8_t delay;

	if (auto_start > 0xFFU) {
		return 0U;
	}
	if (auto_end > 0x100U) {
		auto_end = 0x100U;
	}

	for (auto_len = auto_start; auto_len < auto_end; ++auto_len) {
		uint8_t run_start = 0U;
		uint8_t run_active = 0U;

		spic_disable_rtl8195bhp(dev);
		spic_set_baudr_rtl8195bhp(dev, divider);
		spic_set_fbaudr_rtl8195bhp(dev, divider);
		spic_set_dummy_cycle_rtl8195bhp(dev, (uint8_t)auto_len);
		spic_enable_rtl8195bhp(dev);

		for (delay = 0U; delay <= CARBOX_SPIC_MAX_DELAY; ++delay) {
			uint8_t passed;

			spic_set_delay_line(delay);
			passed = (spic_verify_calibration_para() == _TRUE);
			if (passed && !run_active) {
				run_start = delay;
				run_active = 1U;
			}
			if ((!passed || (delay == CARBOX_SPIC_MAX_DELAY)) && run_active) {
				uint8_t run_end = passed ? (uint8_t)(delay + 1U) : delay;
				uint8_t width = (uint8_t)(run_end - run_start);

				if (width > best_width) {
					best_width = width;
					*best_dummy = (uint8_t)auto_len;
					*best_delay = (uint8_t)(run_start + ((width - 1U) / 2U));
				}
				run_active = 0U;
			}
		}
	}

	return best_width;
}

static int carbox_spic_verify_repeated(uint32_t count)
{
	uint32_t i;

	for (i = 0U; i < count; ++i) {
		if (spic_verify_calibration_para() != _TRUE) {
			return -1;
		}
	}
	return 0;
}

static int carbox_spic_calibrate_sequential(hal_spic_adaptor_t *adaptor,
					    uint8_t divider,
					    uint8_t dummy,
					    uint8_t delay)
{
	SPIC_Type *dev = adaptor->spic_dev;
	spic_auto_length_seq_t seq;
	uint8_t index;
	uint8_t run_start = 0U;
	uint8_t run_active = 0U;
	uint8_t best_start = 0U;
	uint8_t best_width = 0U;

	seq.w = dev->auto_length;
	if (carbox_spic_is_dtr(adaptor->spic_bit_mode)) {
		seq.b.spic_cyc_per_byte = ((divider * 2U * 8U) >> adaptor->data_chnl) >> 1;
	} else {
		seq.b.spic_cyc_per_byte = (divider * 2U * 8U) >> adaptor->data_chnl;
	}
	seq.b.rd_dummy_length = dummy;

	spic_disable_rtl8195bhp(dev);
	dev->valid_cmd_b.seq_trans_en = ENABLE;
	spic_set_delay_line(delay);
	spic_enable_rtl8195bhp(dev);

	for (index = 2U; index < 0x10U; ++index) {
		uint8_t passed;

		seq.b.in_physical_cyc = index;
		dev->auto_length_seq = seq.w;
		passed = (spic_verify_calibration_para() == _TRUE);
		if (passed && !run_active) {
			run_start = index;
			run_active = 1U;
		}
		if ((!passed || (index == 0x0FU)) && run_active) {
			uint8_t run_end = passed ? (uint8_t)(index + 1U) : index;
			uint8_t width = (uint8_t)(run_end - run_start);

			if (width > best_width) {
				best_width = width;
				best_start = run_start;
			}
			run_active = 0U;
		}
	}

	if (best_width == 0U) {
		spic_disable_rtl8195bhp(dev);
		dev->valid_cmd_b.seq_trans_en = DISABLE;
		spic_enable_rtl8195bhp(dev);
		return -1;
	}

	seq.b.in_physical_cyc = best_start + ((best_width - 1U) / 2U);
	dev->auto_length_seq = seq.w;
	if (carbox_spic_verify_repeated(CONFIG_SPIC_CAL_VERIFY_COUNT) != 0) {
		spic_disable_rtl8195bhp(dev);
		dev->valid_cmd_b.seq_trans_en = DISABLE;
		spic_enable_rtl8195bhp(dev);
		return -1;
	}
	adaptor->seq_dum_len[adaptor->spic_bit_mode] =
		(uint8_t)seq.b.in_physical_cyc | 0x10U;
	seq_setting[adaptor->spic_bit_mode][carbox_spic_snapshot.cpu_type].auto_len = dummy;
	seq_setting[adaptor->spic_bit_mode][carbox_spic_snapshot.cpu_type].delay_line = delay;
	return 0;
}

int carbox_spic_overclock_prepare(uint32_t target_sys_hz)
{
#if !CONFIG_SPIC_ADAPTIVE_OVERCLOCK
	(void)target_sys_hz;
	return CARBOX_SPIC_OVERCLOCK_IDLE;
#else
	hal_spic_adaptor_t *adaptor = pglob_spic_adaptor;
	SPIC_Type *dev;
	uint32_t qualified_hz;
	uint32_t provisional_dummy;
	uint8_t divider;
	uint8_t cpu_type;
	uint8_t mode;

	if ((adaptor == NULL) || (adaptor->spic_dev == NULL) ||
	    (adaptor->dummy_cycle == NULL)) {
		carbox_spic_report.status = CARBOX_SPIC_OVERCLOCK_UNAVAILABLE;
		return CARBOX_SPIC_OVERCLOCK_UNAVAILABLE;
	}

	dev = adaptor->spic_dev;
	mode = adaptor->spic_bit_mode;
	cpu_type = spic_query_system_clk();
	if ((mode >= 10U) || (cpu_type >= CPU_CLK_TYPE_NO)) {
		carbox_spic_report.status = CARBOX_SPIC_OVERCLOCK_BAD_CONFIG;
		return CARBOX_SPIC_OVERCLOCK_BAD_CONFIG;
	}

	qualified_hz = carbox_spic_qualified_hz(mode);
	divider = carbox_spic_safe_divider(target_sys_hz, qualified_hz, mode);
	if (divider == 0U) {
		carbox_spic_report.status = CARBOX_SPIC_OVERCLOCK_BAD_CONFIG;
		return CARBOX_SPIC_OVERCLOCK_BAD_CONFIG;
	}

	carbox_spic_snapshot.dev = dev;
	carbox_spic_snapshot.slot = adaptor->spic_init_data[mode][cpu_type];
	carbox_spic_snapshot.baudr = dev->baudr;
	carbox_spic_snapshot.fbaudr = dev->fbaudr;
	carbox_spic_snapshot.auto_length = dev->auto_length;
	carbox_spic_snapshot.auto_length_seq = dev->auto_length_seq;
	carbox_spic_snapshot.valid_cmd = dev->valid_cmd;
	carbox_spic_snapshot.cpu_type = cpu_type;
	carbox_spic_snapshot.io_mode = mode;
	carbox_spic_snapshot.seq_dum_len = adaptor->seq_dum_len[mode];
	carbox_spic_snapshot.active = 1U;

	carbox_spic_report.status = CARBOX_SPIC_OVERCLOCK_PREPARED;
	carbox_spic_report.target_sys_hz = target_sys_hz;
	carbox_spic_report.qualified_max_hz = qualified_hz;
	carbox_spic_report.selected_spic_hz = target_sys_hz / (2U * divider);
	carbox_spic_report.flash_id[0] = adaptor->flash_id[0];
	carbox_spic_report.flash_id[1] = adaptor->flash_id[1];
	carbox_spic_report.flash_id[2] = adaptor->flash_id[2];
	carbox_spic_report.io_mode = mode;
	carbox_spic_report.old_divider = (uint8_t)dev->baudr;
	carbox_spic_report.selected_divider = divider;
	carbox_spic_report.selected_dummy = 0U;
	carbox_spic_report.selected_delay = 0U;
	carbox_spic_report.delay_window = 0U;
	carbox_spic_report.sequential_was_enabled = dev->valid_cmd_b.seq_trans_en ? 1U : 0U;
	carbox_spic_report.sequential_is_enabled = 0U;

	provisional_dummy = (uint32_t)carbox_spic_default_dummy(adaptor) * divider * 2U;
	provisional_dummy += MAX_AUTO_LENGTH - 1U;
	if (provisional_dummy > 0xFFU) {
		provisional_dummy = 0xFFU;
	}

	spic_disable_rtl8195bhp(dev);
	dev->valid_cmd_b.seq_trans_en = DISABLE;
	spic_set_baudr_rtl8195bhp(dev, divider);
	spic_set_fbaudr_rtl8195bhp(dev, divider);
	spic_set_dummy_cycle_rtl8195bhp(dev, (uint8_t)provisional_dummy);
	spic_enable_rtl8195bhp(dev);

	return CARBOX_SPIC_OVERCLOCK_PREPARED;
#endif
}

int carbox_spic_overclock_calibrate(uint32_t target_sys_hz)
{
#if !CONFIG_SPIC_ADAPTIVE_OVERCLOCK
	(void)target_sys_hz;
	return CARBOX_SPIC_OVERCLOCK_IDLE;
#else
	hal_spic_adaptor_t *adaptor = pglob_spic_adaptor;
	SPIC_Type *dev;
	uint8_t divider;
	uint8_t start_divider;
	uint8_t default_dummy;

	if (!carbox_spic_snapshot.active || (adaptor == NULL) ||
	    (adaptor->spic_dev != carbox_spic_snapshot.dev) ||
	    (adaptor->spic_bit_mode != carbox_spic_snapshot.io_mode) ||
	    (target_sys_hz != carbox_spic_report.target_sys_hz)) {
		carbox_spic_report.status = CARBOX_SPIC_OVERCLOCK_BAD_CONFIG;
		return CARBOX_SPIC_OVERCLOCK_BAD_CONFIG;
	}

	dev = adaptor->spic_dev;
	default_dummy = carbox_spic_default_dummy(adaptor);
	start_divider = carbox_spic_report.selected_divider;
	for (divider = start_divider; divider <= CARBOX_SPIC_MAX_DIV; ++divider) {
		uint8_t dummy = 0U;
		uint8_t delay = 0U;
		uint8_t window;

		window = carbox_spic_find_window(dev, divider, default_dummy,
						  &dummy, &delay);
		if (window < CONFIG_SPIC_CAL_MIN_WINDOW) {
			continue;
		}

		carbox_spic_apply(dev, divider, dummy, delay);
		if (carbox_spic_verify_repeated(CONFIG_SPIC_CAL_VERIFY_COUNT) != 0) {
			continue;
		}

		adaptor->spic_init_data[adaptor->spic_bit_mode]
			[carbox_spic_snapshot.cpu_type].baud_rate = divider;
		adaptor->spic_init_data[adaptor->spic_bit_mode]
			[carbox_spic_snapshot.cpu_type].rd_dummy_cycle = dummy;
		adaptor->spic_init_data[adaptor->spic_bit_mode]
			[carbox_spic_snapshot.cpu_type].delay_line = delay;
		adaptor->spic_init_data[adaptor->spic_bit_mode]
			[carbox_spic_snapshot.cpu_type].valid = 1U;

		carbox_spic_report.selected_divider = divider;
		carbox_spic_report.selected_dummy = dummy;
		carbox_spic_report.selected_delay = delay;
		carbox_spic_report.delay_window = window;
		carbox_spic_report.selected_spic_hz = target_sys_hz / (2U * divider);

		if (carbox_spic_report.sequential_was_enabled &&
		    (carbox_spic_calibrate_sequential(adaptor, divider, dummy, delay) == 0)) {
			carbox_spic_report.sequential_is_enabled = 1U;
		}

		carbox_spic_report.status = CARBOX_SPIC_OVERCLOCK_CALIBRATED;
		carbox_spic_snapshot.active = 0U;
		return CARBOX_SPIC_OVERCLOCK_CALIBRATED;
	}

	carbox_spic_report.status = CARBOX_SPIC_OVERCLOCK_NO_WINDOW;
	return CARBOX_SPIC_OVERCLOCK_NO_WINDOW;
#endif
}

void carbox_spic_overclock_restore(void)
{
#if CONFIG_SPIC_ADAPTIVE_OVERCLOCK
	hal_spic_adaptor_t *adaptor = pglob_spic_adaptor;
	SPIC_Type *dev = carbox_spic_snapshot.dev;

	if (!carbox_spic_snapshot.active || (adaptor == NULL) || (dev == NULL)) {
		return;
	}

	spic_disable_rtl8195bhp(dev);
	dev->baudr = carbox_spic_snapshot.baudr;
	dev->fbaudr = carbox_spic_snapshot.fbaudr;
	dev->auto_length = carbox_spic_snapshot.auto_length;
	dev->auto_length_seq = carbox_spic_snapshot.auto_length_seq;
	dev->valid_cmd = carbox_spic_snapshot.valid_cmd;
	spic_enable_rtl8195bhp(dev);
	adaptor->spic_init_data[carbox_spic_snapshot.io_mode]
		[carbox_spic_snapshot.cpu_type] = carbox_spic_snapshot.slot;
	adaptor->seq_dum_len[carbox_spic_snapshot.io_mode] =
		carbox_spic_snapshot.seq_dum_len;
	carbox_spic_snapshot.active = 0U;
#endif
}

void carbox_spic_overclock_get_report(struct carbox_spic_overclock_report *report)
{
	if (report != NULL) {
		*report = carbox_spic_report;
	}
}
