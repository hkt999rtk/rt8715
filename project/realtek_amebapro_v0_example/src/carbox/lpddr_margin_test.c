#include "lpddr_margin_test.h"

#include "hal_cache.h"
#include "rtl8195bhp.h"

#ifndef CONFIG_LPDDR_MARGIN_TEST
#define CONFIG_LPDDR_MARGIN_TEST 0
#endif


#define LPDDR_MARGIN_BUFFER_BYTES (64U * 1024U)
#define LPDDR_MARGIN_WORDS (LPDDR_MARGIN_BUFFER_BYTES / sizeof(uint32_t))
#define LPDDR_MARGIN_PATTERNS 8U
#define LPDDR_MARGIN_MAX_RECORDED_ERRORS 16U

#define LPDDR_MARGIN_SRAM_TEXT \
	__attribute__((section(".sram.text.lpddr_margin"), noinline))
#define LPDDR_MARGIN_SRAM_DATA \
	__attribute__((section(".sram.bss.lpddr_margin"), aligned(32)))
#define LPDDR_MARGIN_LPDDR_DATA \
	__attribute__((section(".lpddr.bss.lpddr_margin_test"), aligned(32)))

#if CONFIG_LPDDR_MARGIN_TEST

static volatile uint32_t lpddr_margin_buffer[LPDDR_MARGIN_WORDS]
	LPDDR_MARGIN_LPDDR_DATA;

static volatile struct carbox_lpddr_margin_result lpddr_margin_result
	LPDDR_MARGIN_SRAM_DATA;


static inline __attribute__((always_inline)) uint32_t
lpddr_margin_pattern(uint32_t pass, uint32_t index)
{
	switch (pass) {
	case 0U:
		return 0x00000000U;
	case 1U:
		return 0xFFFFFFFFU;
	case 2U:
		return 0xAAAAAAAAU;
	case 3U:
		return 0x55555555U;
	case 4U:
		return ((uint32_t)&lpddr_margin_buffer[index]) ^ 0xA5A5A5A5U;
	case 5U:
		return ~(((uint32_t)&lpddr_margin_buffer[index]) ^ 0xA5A5A5A5U);
	case 6U:
		return 1UL << (index & 31U);
	default:
		return ~(1UL << (index & 31U));
	}
}

LPDDR_MARGIN_SRAM_TEXT void carbox_lpddr_margin_test_run_once(void)
{
	uint32_t pass;
	uint32_t index;

	if (lpddr_margin_result.state != CARBOX_LPDDR_MARGIN_DISABLED) {
		return;
	}

	lpddr_margin_result.magic = CARBOX_LPDDR_MARGIN_MAGIC;
	lpddr_margin_result.version = CARBOX_LPDDR_MARGIN_VERSION;
	lpddr_margin_result.state = CARBOX_LPDDR_MARGIN_RUNNING;
	lpddr_margin_result.runs = 1U;
	lpddr_margin_result.buffer_address = (uint32_t)lpddr_margin_buffer;
	lpddr_margin_result.buffer_bytes = LPDDR_MARGIN_BUFFER_BYTES;
	lpddr_margin_result.patterns_completed = 0U;
	lpddr_margin_result.words_checked = 0U;
	lpddr_margin_result.error_count = 0U;
	lpddr_margin_result.first_error_address = 0U;
	lpddr_margin_result.first_expected = 0U;
	lpddr_margin_result.first_actual = 0U;

	for (pass = 0U; pass < LPDDR_MARGIN_PATTERNS; pass++) {
		for (index = 0U; index < LPDDR_MARGIN_WORDS; index++) {
			lpddr_margin_buffer[index] = lpddr_margin_pattern(pass, index);
		}

		__DMB();
		dcache_clean_by_addr((uint32_t *)lpddr_margin_buffer,
			(int32_t)LPDDR_MARGIN_BUFFER_BYTES);
		__DSB();
		dcache_invalidate_by_addr((uint32_t *)lpddr_margin_buffer,
			(int32_t)LPDDR_MARGIN_BUFFER_BYTES);
		__DSB();

		for (index = 0U; index < LPDDR_MARGIN_WORDS; index++) {
			uint32_t expected = lpddr_margin_pattern(pass, index);
			uint32_t actual = lpddr_margin_buffer[index];

			lpddr_margin_result.words_checked++;
			if (actual != expected) {
				if (lpddr_margin_result.error_count == 0U) {
					lpddr_margin_result.first_error_address =
						(uint32_t)&lpddr_margin_buffer[index];
					lpddr_margin_result.first_expected = expected;
					lpddr_margin_result.first_actual = actual;
				}
				lpddr_margin_result.error_count++;
				if (lpddr_margin_result.error_count >=
				    LPDDR_MARGIN_MAX_RECORDED_ERRORS) {
					lpddr_margin_result.state = CARBOX_LPDDR_MARGIN_FAIL;
					return;
				}
			}
		}
		lpddr_margin_result.patterns_completed++;
	}

	lpddr_margin_result.state = (lpddr_margin_result.error_count == 0U) ?
		CARBOX_LPDDR_MARGIN_PASS : CARBOX_LPDDR_MARGIN_FAIL;
}

const volatile struct carbox_lpddr_margin_result *
carbox_lpddr_margin_test_get_result(void)
{
	return &lpddr_margin_result;
}

#else

void carbox_lpddr_margin_test_run_once(void)
{
}

const volatile struct carbox_lpddr_margin_result *
carbox_lpddr_margin_test_get_result(void)
{
	return (const volatile struct carbox_lpddr_margin_result *)0;
}

#endif

void carbox_lpddr_margin_test_report(uint32_t sequence)
{
	const volatile struct carbox_lpddr_margin_result *result =
		carbox_lpddr_margin_test_get_result();

	if (result == 0 || result->magic != CARBOX_LPDDR_MARGIN_MAGIC) {
		return;
	}

	rt_printf("[LPDDRMARGIN][%lu] version/state/runs=%lu/%lu/%lu "
		  "buffer=%08lx/%luB patterns=%lu/%lu words=%lu errors=%lu "
		  "first addr/expected/actual=%08lx/%08lx/%08lx "
		  "fixed_phase=1 reserved_buffer=1\r\n",
		  (unsigned long)sequence,
		  (unsigned long)result->version,
		  (unsigned long)result->state,
		  (unsigned long)result->runs,
		  (unsigned long)result->buffer_address,
		  (unsigned long)result->buffer_bytes,
		  (unsigned long)result->patterns_completed,
		  (unsigned long)LPDDR_MARGIN_PATTERNS,
		  (unsigned long)result->words_checked,
		  (unsigned long)result->error_count,
		  (unsigned long)result->first_error_address,
		  (unsigned long)result->first_expected,
		  (unsigned long)result->first_actual);
}
