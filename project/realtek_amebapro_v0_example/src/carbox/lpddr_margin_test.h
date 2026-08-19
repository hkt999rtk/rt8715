#ifndef CARBOX_LPDDR_MARGIN_TEST_H
#define CARBOX_LPDDR_MARGIN_TEST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CARBOX_LPDDR_MARGIN_MAGIC 0x4C504D54UL /* "LPMT" */
#define CARBOX_LPDDR_MARGIN_VERSION 1U

enum carbox_lpddr_margin_state {
	CARBOX_LPDDR_MARGIN_DISABLED = 0,
	CARBOX_LPDDR_MARGIN_RUNNING = 1,
	CARBOX_LPDDR_MARGIN_PASS = 2,
	CARBOX_LPDDR_MARGIN_FAIL = 3,
};

struct carbox_lpddr_margin_result {
	uint32_t magic;
	uint32_t version;
	uint32_t state;
	uint32_t runs;
	uint32_t buffer_address;
	uint32_t buffer_bytes;
	uint32_t patterns_completed;
	uint32_t words_checked;
	uint32_t error_count;
	uint32_t first_error_address;
	uint32_t first_expected;
	uint32_t first_actual;
};

void carbox_lpddr_margin_test_run_once(void);
const volatile struct carbox_lpddr_margin_result *
carbox_lpddr_margin_test_get_result(void);
void carbox_lpddr_margin_test_report(uint32_t sequence);

#ifdef __cplusplus
}
#endif

#endif
