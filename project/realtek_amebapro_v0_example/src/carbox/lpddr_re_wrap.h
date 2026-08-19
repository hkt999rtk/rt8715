#ifndef CARBOX_LPDDR_RE_WRAP_H
#define CARBOX_LPDDR_RE_WRAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CARBOX_LPDDR_RE_MAGIC 0x4C504452UL /* "LPDR" */
#define CARBOX_LPDDR_RE_VERSION 7U

struct carbox_lpddr_re_record {
	uint32_t magic;
	uint32_t version;
	uint32_t complete;
	uint32_t init_calls;
	uint32_t call_order;
	uint32_t vendor_period_ps;
	uint32_t dram_period_ps;
	uint32_t clock_override_hz;
	uint32_t device;
	uint32_t mode;
	uint32_t timing;
	uint32_t phase_calls;
	uint32_t phy_init_calls;
	uint32_t pll_reapply_calls;
	uint32_t phase_override;
	uint32_t phase_reapply_calls;
	int32_t oesync_ck;
	int32_t oesync_dqs;
	int32_t oesync_dq;
	int32_t wrlvl_dqs;
	int32_t wrlvl_dq;
	uint32_t dpi_before[7];
	uint32_t dpi_after[7];
	uint32_t phase_before[4];
	uint32_t phase_after[4];
	uint32_t phase_gate_off[2];
	uint32_t phase_gate_restored[2];
	uint32_t ctrl_after[12];
};

const volatile struct carbox_lpddr_re_record *
carbox_lpddr_re_get_record(void);

#ifdef __cplusplus
}
#endif

#endif
