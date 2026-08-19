#include "irq_profiler.h"
#include "ncm/usb_boot_profiler.h"

#include <stdint.h>
#include <string.h>

#include "cmsis.h"
#include "diag.h"
#include "hal_irq.h"

#ifndef CONFIG_IRQ_PROFILE
#define CONFIG_IRQ_PROFILE 0
#endif

#ifndef CONFIG_IRQ_PROFILE_REPORT
#define CONFIG_IRQ_PROFILE_REPORT 0
#endif

#ifndef CONFIG_IRQ_PROFILE_USB_CAUSE
#define CONFIG_IRQ_PROFILE_USB_CAUSE 0
#endif

#ifndef CONFIG_IRQ_PROFILE_USB_HANDOFF
#define CONFIG_IRQ_PROFILE_USB_HANDOFF 0
#endif

#ifndef CONFIG_IRQ_PROFILE_USB_CH4_FLOW
#define CONFIG_IRQ_PROFILE_USB_CH4_FLOW 0
#endif

#ifndef CONFIG_IRQ_PROFILE_USB_CH4_NCM
#define CONFIG_IRQ_PROFILE_USB_CH4_NCM 0
#endif

#ifndef CONFIG_IRQ_PROFILE_USB_CH4_SEQUENCE
#define CONFIG_IRQ_PROFILE_USB_CH4_SEQUENCE 0
#endif

#ifndef CONFIG_USB_IRQ_CLEAR_STALE_PENDING
#define CONFIG_USB_IRQ_CLEAR_STALE_PENDING 0
#endif

#ifndef CONFIG_USB_IRQ_SAFE_DEDUP
#define CONFIG_USB_IRQ_SAFE_DEDUP 0
#endif

#ifndef CONFIG_USB_IRQ_CH3_ACK_FASTPATH
#define CONFIG_USB_IRQ_CH3_ACK_FASTPATH 0
#endif

#ifndef CONFIG_USB_IRQ_CH4_NAK_COALESCE
#define CONFIG_USB_IRQ_CH4_NAK_COALESCE 0
#endif

#ifndef USB_IRQ_CH4_NAK_COALESCE_TIMEOUT_US
#define USB_IRQ_CH4_NAK_COALESCE_TIMEOUT_US 50000U
#endif

#if CONFIG_USB_IRQ_SAFE_DEDUP && CONFIG_USB_IRQ_CLEAR_STALE_PENDING
#error "Select USB_IRQ_SAFE_DEDUP or the legacy stale-pending experiment, not both"
#endif

#if CONFIG_USB_IRQ_CH4_NAK_COALESCE && !CONFIG_IRQ_PROFILE
#error "USB IRQ CH4 NAK coalescing requires the IRQ profiler counter banks"
#endif

#if CONFIG_IRQ_PROFILE

#define IRQPROF_IRQ_COUNT 32U
#define IRQPROF_TOP_COUNT 8U
#define IRQPROF_USB_IRQ 12U
#define IRQPROF_USB_GINTSTS_ADDR 0x400C0014U
#define IRQPROF_USB_GINTMSK_ADDR 0x400C0018U

#if CONFIG_IRQ_PROFILE_USB_CAUSE || CONFIG_USB_IRQ_CH3_ACK_FASTPATH || \
	CONFIG_USB_IRQ_CH4_NAK_COALESCE
#define IRQPROF_USB_HAINT_ADDR    0x400C0414U
#define IRQPROF_USB_HAINTMSK_ADDR 0x400C0418U
#define IRQPROF_USB_HCINT_BASE    0x400C0508U
#define IRQPROF_USB_HCINT_STRIDE  0x20U
#define IRQPROF_USB_HCINTMSK_OFF  0x04U
#define IRQPROF_USB_HCINT_MASK    (1U << 25)
#endif

#if CONFIG_USB_IRQ_CH4_NAK_COALESCE
enum irqprof_usb_ch4_coalesce_counter {
	IRQPROF_USB_CH4_COALESCE_DEFER,
	IRQPROF_USB_CH4_COALESCE_MERGE,
	IRQPROF_USB_CH4_COALESCE_TIMEOUT,
	IRQPROF_USB_CH4_COALESCE_HALT_CLEARED_NAK,
	IRQPROF_USB_CH4_COALESCE_COUNTER_COUNT,
};
#endif

#if CONFIG_IRQ_PROFILE_USB_CH4_NCM
struct irqprof_usb_ch4_ncm_stats {
	volatile uint32_t get_calls;
	volatile uint32_t get_ok;
	volatile uint32_t get_fail;
	volatile uint32_t device_in_max;
	volatile uint32_t device_out_max;
	volatile uint32_t set_calls;
	volatile uint32_t set_ok;
	volatile uint32_t set_fail;
	volatile uint32_t set_requested;
	volatile uint32_t receive_size;
	volatile uint32_t receive_size_calls;
};

static struct irqprof_usb_ch4_ncm_stats irqprof_usb_ch4_ncm;

static uint32_t irqprof_get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
#endif

#if CONFIG_IRQ_PROFILE_USB_HANDOFF
enum irqprof_usb_handoff_counter {
	IRQPROF_USB_HANDOFF_GIVE,
	IRQPROF_USB_HANDOFF_GIVE_OK,
	IRQPROF_USB_HANDOFF_GIVE_FAIL,
	IRQPROF_USB_HANDOFF_YIELD,
	IRQPROF_USB_HANDOFF_NO_YIELD,
	IRQPROF_USB_HANDOFF_COUNTER_COUNT,
};

#if CONFIG_IRQ_PROFILE_USB_CAUSE
enum irqprof_usb_channel_handoff_counter {
	IRQPROF_USB_CHANNEL_HANDOFF_GIVE,
	IRQPROF_USB_CHANNEL_HANDOFF_YIELD,
	IRQPROF_USB_CHANNEL_HANDOFF_COUNTER_COUNT,
};

enum irqprof_usb_ch4_halt_gap_counter {
	IRQPROF_USB_CH4_GAP_SAMPLE,
	IRQPROF_USB_CH4_GAP_LE_2US,
	IRQPROF_USB_CH4_GAP_LE_5US,
	IRQPROF_USB_CH4_GAP_LE_10US,
	IRQPROF_USB_CH4_GAP_LE_20US,
	IRQPROF_USB_CH4_GAP_LE_50US,
	IRQPROF_USB_CH4_GAP_LE_100US,
	IRQPROF_USB_CH4_GAP_GT_100US,
	IRQPROF_USB_CH4_GAP_MAX_CYCLES,
	IRQPROF_USB_CH4_GAP_COUNTER_COUNT,
};

#if CONFIG_IRQ_PROFILE_USB_CH4_FLOW
enum irqprof_usb_ch4_flow_counter {
	IRQPROF_USB_CH4_NAK_SERVICE,
	IRQPROF_USB_CH4_NAK_SERVICE_CYCLES,
	IRQPROF_USB_CH4_NAK_SERVICE_MAX_CYCLES,
	IRQPROF_USB_CH4_HALT_SERVICE,
	IRQPROF_USB_CH4_HALT_SERVICE_CYCLES,
	IRQPROF_USB_CH4_HALT_SERVICE_MAX_CYCLES,
	IRQPROF_USB_CH4_OTHER_SERVICE,
	IRQPROF_USB_CH4_NAK_HALT_CALL,
	IRQPROF_USB_CH4_OTHER_HALT_CALL,
	IRQPROF_USB_CH4_FLOW_COUNTER_COUNT,
};
#endif
#endif
#endif

#if CONFIG_USB_IRQ_SAFE_DEDUP
enum irqprof_usb_dedup_counter {
	IRQPROF_USB_DEDUP_CALL,
	IRQPROF_USB_DEDUP_NVIC_PENDING,
	IRQPROF_USB_DEDUP_CONTROLLER_PENDING,
	IRQPROF_USB_DEDUP_REPEND,
	IRQPROF_USB_DEDUP_COUNTER_COUNT,
};
#endif

#if CONFIG_IRQ_PROFILE_USB_CAUSE
#define IRQPROF_USB_SOF_MASK     (1U << 3)
#define IRQPROF_USB_PORT_MASK    (1U << 24)
#define IRQPROF_USB_KNOWN_MASK   (IRQPROF_USB_SOF_MASK | \
				  IRQPROF_USB_PORT_MASK | \
				  IRQPROF_USB_HCINT_MASK)

enum irqprof_usb_counter {
	IRQPROF_USB_SAMPLE,
	IRQPROF_USB_ZERO,
	IRQPROF_USB_SOF,
	IRQPROF_USB_SOF_ONLY,
	IRQPROF_USB_HCINT,
	IRQPROF_USB_HCINT_ONLY,
	IRQPROF_USB_PORT,
	IRQPROF_USB_OTHER,
	IRQPROF_USB_MULTI,
	IRQPROF_USB_HC_NO_CHANNEL,
	IRQPROF_USB_COUNTER_COUNT,
};

#define IRQPROF_USB_CHANNEL_COUNT 16U
#define IRQPROF_USB_HC_REASON_COUNT 14U

enum irqprof_usb_ch3_ack_counter {
	IRQPROF_USB_CH3_ACK,
	IRQPROF_USB_CH3_ACK_DOPING,
	IRQPROF_USB_CH3_ACK_FAST_DROP,
	IRQPROF_USB_CH3_ACK_COUNTER_COUNT,
};

#if CONFIG_IRQ_PROFILE_USB_CH4_SEQUENCE
enum irqprof_usb_ch4_sequence_counter {
	IRQPROF_USB_CH4_SEQ_NAK,
	IRQPROF_USB_CH4_SEQ_SAME_IRQ,
	IRQPROF_USB_CH4_SEQ_SUBMIT_FIRST,
	IRQPROF_USB_CH4_SEQ_HALT_FIRST,
	IRQPROF_USB_CH4_SEQ_NAK_OVERLAP,
	IRQPROF_USB_CH4_SEQ_SUBMIT_US,
	IRQPROF_USB_CH4_SEQ_SUBMIT_MAX_US,
	IRQPROF_USB_CH4_SEQ_HALT_US,
	IRQPROF_USB_CH4_SEQ_HALT_MAX_US,
	IRQPROF_USB_CH4_SEQ_COUNTER_COUNT,
};
#endif

#if CONFIG_USB_IRQ_CLEAR_STALE_PENDING
enum irqprof_usb_reenable_counter {
	IRQPROF_USB_REENABLE_CALL,
	IRQPROF_USB_REENABLE_CLEAR,
	IRQPROF_USB_REENABLE_PENDING,
	IRQPROF_USB_REENABLE_COUNTER_COUNT,
};
#endif
#endif

/*
 * IRQ names follow rtl8195bhp.h.  Keep the IRQ number in the report as the
 * authoritative identifier in case a future chip revision renames a block.
 */
static const char *const irqprof_names[IRQPROF_IRQ_COUNT] = {
	"SystemOn", "TimerGroup0", "TimerGroup1", "GPIO",
	"PWM", "ADC", "SGPIO", "UART",
	"I2C", "SSI", "I2S", "I3C",
	"USB", "SDIOH", "SDIOD", "ETHERNET",
	"WLAN", "GDMA0", "GDMA1", "Crypto",
	"SPIC", "ICC", "ISP", "H264",
	"VOE", "TFT", "MJPG", "SGDMA0",
	"SGDMA1", "SCrypto", "SLowPri", "LowPri",
};

/*
 * Two banks let the reporting task rotate counters without putting a lock in
 * any ISR.  The handler table and counters stay in internal RAM; the common
 * trampoline itself is placed in ITCM.
 */
static volatile uint32_t irqprof_handlers[IRQPROF_IRQ_COUNT]
	__attribute__((used));
static volatile uint32_t irqprof_counts[2][IRQPROF_IRQ_COUNT]
	__attribute__((used));
static volatile uint32_t irqprof_active_bank __attribute__((used));
static uint32_t irqprof_snapshot[IRQPROF_IRQ_COUNT];
#if CONFIG_IRQ_PROFILE_USB_HANDOFF
static volatile uint32_t
	irqprof_usb_handoff_counts[2][IRQPROF_USB_HANDOFF_COUNTER_COUNT]
	__attribute__((used));
static uint32_t
	irqprof_usb_handoff_snapshot[IRQPROF_USB_HANDOFF_COUNTER_COUNT];
#if CONFIG_IRQ_PROFILE_USB_CAUSE
static volatile uint32_t irqprof_usb_current_channels;
static volatile uint32_t irqprof_usb_current_ch4_reasons;
static volatile uint32_t irqprof_usb_ch4_reenable_cycles;
static volatile uint32_t irqprof_usb_ch4_reenable_valid;
static volatile uint32_t
	irqprof_usb_channel_handoff_counts[2][IRQPROF_USB_CHANNEL_COUNT]
		[IRQPROF_USB_CHANNEL_HANDOFF_COUNTER_COUNT]
	__attribute__((used));
static uint32_t
	irqprof_usb_channel_handoff_snapshot[IRQPROF_USB_CHANNEL_COUNT]
		[IRQPROF_USB_CHANNEL_HANDOFF_COUNTER_COUNT];
static volatile uint32_t
	irqprof_usb_ch4_halt_gap_counts[2][IRQPROF_USB_CH4_GAP_COUNTER_COUNT]
	__attribute__((used));
static uint32_t
	irqprof_usb_ch4_halt_gap_snapshot[IRQPROF_USB_CH4_GAP_COUNTER_COUNT];
#if CONFIG_IRQ_PROFILE_USB_CH4_FLOW
static volatile uint32_t irqprof_usb_ch4_service_start_cycles;
static volatile uint32_t irqprof_usb_ch4_service_reasons;
static volatile uint32_t irqprof_usb_ch4_service_valid;
static volatile uint32_t
	irqprof_usb_ch4_flow_counts[2][IRQPROF_USB_CH4_FLOW_COUNTER_COUNT]
	__attribute__((used));
static uint32_t
	irqprof_usb_ch4_flow_snapshot[IRQPROF_USB_CH4_FLOW_COUNTER_COUNT];
#endif
#endif
#endif
#if CONFIG_IRQ_PROFILE_USB_HANDOFF || CONFIG_USB_IRQ_SAFE_DEDUP
static volatile uint32_t irqprof_usb_handoff_reenable_expected;
#endif
#if CONFIG_USB_IRQ_SAFE_DEDUP
static volatile uint32_t
	irqprof_usb_dedup_counts[2][IRQPROF_USB_DEDUP_COUNTER_COUNT]
	__attribute__((used));
static uint32_t irqprof_usb_dedup_snapshot[IRQPROF_USB_DEDUP_COUNTER_COUNT];
#endif
#if CONFIG_USB_IRQ_CH4_NAK_COALESCE
static volatile uint32_t irqprof_usb_ch4_coalesce_pending;
static volatile uint32_t irqprof_usb_ch4_coalesce_start_cycles;
static volatile uint32_t
	irqprof_usb_ch4_coalesce_counts[2][IRQPROF_USB_CH4_COALESCE_COUNTER_COUNT]
	__attribute__((used));
static uint32_t
	irqprof_usb_ch4_coalesce_snapshot[IRQPROF_USB_CH4_COALESCE_COUNTER_COUNT];
#endif
#if CONFIG_IRQ_PROFILE_USB_CAUSE
static volatile uint32_t
	irqprof_usb_counts[2][IRQPROF_USB_COUNTER_COUNT] __attribute__((used));
static volatile uint32_t
	irqprof_usb_channel_counts[2][IRQPROF_USB_CHANNEL_COUNT]
	__attribute__((used));
static volatile uint32_t
	irqprof_usb_hc_reason_counts[2][IRQPROF_USB_HC_REASON_COUNT]
	__attribute__((used));
static volatile uint32_t
	irqprof_usb_channel_reason_counts[2][IRQPROF_USB_CHANNEL_COUNT]
		[IRQPROF_USB_HC_REASON_COUNT] __attribute__((used));
static volatile uint32_t
	irqprof_usb_channel_hcchar[2][IRQPROF_USB_CHANNEL_COUNT]
	__attribute__((used));
static volatile uint32_t
	irqprof_usb_ch3_ack_counts[2][IRQPROF_USB_CH3_ACK_COUNTER_COUNT]
	__attribute__((used));
#if CONFIG_USB_IRQ_CLEAR_STALE_PENDING
static volatile uint32_t
	irqprof_usb_reenable_counts[2][IRQPROF_USB_REENABLE_COUNTER_COUNT]
	__attribute__((used));
#endif
static uint32_t irqprof_usb_snapshot[IRQPROF_USB_COUNTER_COUNT];
static uint32_t irqprof_usb_channel_snapshot[IRQPROF_USB_CHANNEL_COUNT];
static uint32_t irqprof_usb_hc_reason_snapshot[IRQPROF_USB_HC_REASON_COUNT];
static uint32_t
	irqprof_usb_channel_reason_snapshot[IRQPROF_USB_CHANNEL_COUNT]
		[IRQPROF_USB_HC_REASON_COUNT];
static uint32_t irqprof_usb_channel_hcchar_snapshot[IRQPROF_USB_CHANNEL_COUNT];
static uint32_t
	irqprof_usb_ch3_ack_snapshot[IRQPROF_USB_CH3_ACK_COUNTER_COUNT];
#if CONFIG_IRQ_PROFILE_USB_CH4_SEQUENCE
static volatile uint32_t irqprof_usb_ch4_sequence_pending;
static volatile uint32_t irqprof_usb_ch4_sequence_start_cycles;
static volatile uint32_t
	irqprof_usb_ch4_sequence_counts[2][IRQPROF_USB_CH4_SEQ_COUNTER_COUNT]
	__attribute__((used));
static uint32_t
	irqprof_usb_ch4_sequence_snapshot[IRQPROF_USB_CH4_SEQ_COUNTER_COUNT];
#endif
#if CONFIG_USB_IRQ_CLEAR_STALE_PENDING
static uint32_t
	irqprof_usb_reenable_snapshot[IRQPROF_USB_REENABLE_COUNTER_COUNT];
#endif

#if CONFIG_IRQ_PROFILE_USB_CH4_SEQUENCE
static void irqprof_usb_ch4_sequence_record(uint32_t count_index,
					     uint32_t us_index,
					     uint32_t max_index,
					     uint32_t now)
{
	volatile uint32_t *sequence =
		irqprof_usb_ch4_sequence_counts[irqprof_active_bank];
	uint32_t delta = now - irqprof_usb_ch4_sequence_start_cycles;
	uint32_t cycles_per_us = SystemCoreClock / 1000000U;
	uint32_t delta_us = cycles_per_us != 0U ? delta / cycles_per_us : 0U;

	sequence[count_index]++;
	sequence[us_index] += delta_us;
	if (delta_us > sequence[max_index]) {
		sequence[max_index] = delta_us;
	}
	irqprof_usb_ch4_sequence_pending = 0U;
}

void carbox_irq_profiler_usb_ch4_submit(void)
{
	uint32_t primask;

	if (irqprof_usb_ch4_sequence_pending == 0U) {
		return;
	}

	primask = __get_PRIMASK();
	__disable_irq();
	if (irqprof_usb_ch4_sequence_pending != 0U) {
		irqprof_usb_ch4_sequence_record(
			IRQPROF_USB_CH4_SEQ_SUBMIT_FIRST,
			IRQPROF_USB_CH4_SEQ_SUBMIT_US,
			IRQPROF_USB_CH4_SEQ_SUBMIT_MAX_US,
			DWT->CYCCNT);
	}
	__set_PRIMASK(primask);
}
#else
void carbox_irq_profiler_usb_ch4_submit(void)
{
}
#endif

/*
 * Called only for USB IRQ 12 by the ITCM trampoline.  Reading the masked
 * pending bits before the vendor top half disables/clears the controller lets
 * us distinguish periodic SOF interrupts from transfer completion (HCINT),
 * port events, and other DWC causes.  No lock is needed: snapshot rotation is
 * performed with peripheral interrupts masked.
 */
static void __attribute__((noinline, used,
	section(".itcm.text.irqprof_usb_cause_sample")))
irqprof_usb_cause_sample(void)
{
	volatile uint32_t *counts = irqprof_usb_counts[irqprof_active_bank];
	uint32_t status = *(volatile uint32_t *)(uintptr_t)
		IRQPROF_USB_GINTSTS_ADDR;
	uint32_t mask = *(volatile uint32_t *)(uintptr_t)
		IRQPROF_USB_GINTMSK_ADDR;
	uint32_t pending = status & mask;
	uint32_t categories = 0U;

#if CONFIG_IRQ_PROFILE_USB_HANDOFF
	/* Consumed later in this same IRQ by the HCD semaphore-give wrapper. */
	irqprof_usb_current_channels = 0U;
	irqprof_usb_current_ch4_reasons = 0U;
#endif

	counts[IRQPROF_USB_SAMPLE]++;
	if (pending == 0U) {
		counts[IRQPROF_USB_ZERO]++;
		return;
	}
	if ((pending & IRQPROF_USB_SOF_MASK) != 0U) {
		counts[IRQPROF_USB_SOF]++;
		categories++;
	}
	if ((pending & IRQPROF_USB_HCINT_MASK) != 0U) {
		uint32_t bank = irqprof_active_bank;
		uint32_t channels =
			*(volatile uint32_t *)(uintptr_t)IRQPROF_USB_HAINT_ADDR &
			*(volatile uint32_t *)(uintptr_t)IRQPROF_USB_HAINTMSK_ADDR &
			0xFFFFU;
		uint32_t channel;

#if CONFIG_IRQ_PROFILE_USB_HANDOFF
		irqprof_usb_current_channels = channels;
#endif

		counts[IRQPROF_USB_HCINT]++;
		categories++;
		if (channels == 0U) {
			counts[IRQPROF_USB_HC_NO_CHANNEL]++;
		}
		for (channel = 0U; channel < IRQPROF_USB_CHANNEL_COUNT;
		     ++channel) {
			uint32_t bit = 1U << channel;
			uint32_t hcint_addr;
			uint32_t reasons;
			uint32_t reason;

			if ((channels & bit) == 0U) {
				continue;
			}
			irqprof_usb_channel_counts[bank][channel]++;
			hcint_addr = IRQPROF_USB_HCINT_BASE +
				channel * IRQPROF_USB_HCINT_STRIDE;
			irqprof_usb_channel_hcchar[bank][channel] =
				*(volatile uint32_t *)(uintptr_t)(hcint_addr - 8U);
			reasons =
				*(volatile uint32_t *)(uintptr_t)hcint_addr &
				*(volatile uint32_t *)(uintptr_t)
					(hcint_addr + IRQPROF_USB_HCINTMSK_OFF);
#if CONFIG_IRQ_PROFILE_USB_HANDOFF
			if (channel == 4U) {
				irqprof_usb_current_ch4_reasons = reasons;
#if CONFIG_IRQ_PROFILE_USB_CH4_SEQUENCE
				{
					volatile uint32_t *sequence =
						irqprof_usb_ch4_sequence_counts[bank];
					uint32_t now = DWT->CYCCNT;
					uint32_t nak = reasons & (1U << 4);
					uint32_t halt = reasons & (1U << 1);

					if (nak != 0U && halt != 0U) {
						sequence[IRQPROF_USB_CH4_SEQ_NAK]++;
						sequence[IRQPROF_USB_CH4_SEQ_SAME_IRQ]++;
						irqprof_usb_ch4_sequence_pending = 0U;
					} else {
						if (halt != 0U &&
						    irqprof_usb_ch4_sequence_pending != 0U) {
							irqprof_usb_ch4_sequence_record(
								IRQPROF_USB_CH4_SEQ_HALT_FIRST,
								IRQPROF_USB_CH4_SEQ_HALT_US,
								IRQPROF_USB_CH4_SEQ_HALT_MAX_US,
								now);
						}
						if (nak != 0U) {
							sequence[IRQPROF_USB_CH4_SEQ_NAK]++;
							if (irqprof_usb_ch4_sequence_pending != 0U) {
								sequence[IRQPROF_USB_CH4_SEQ_NAK_OVERLAP]++;
							} else {
								irqprof_usb_ch4_sequence_start_cycles = now;
								irqprof_usb_ch4_sequence_pending = 1U;
							}
						}
					}
				}
#endif
				if ((reasons & (1U << 1)) != 0U &&
				    irqprof_usb_ch4_reenable_valid != 0U) {
					volatile uint32_t *gap =
						irqprof_usb_ch4_halt_gap_counts[bank];
					uint32_t delta = DWT->CYCCNT -
						irqprof_usb_ch4_reenable_cycles;
					uint32_t cycles_per_us = SystemCoreClock /
						1000000U;

					irqprof_usb_ch4_reenable_valid = 0U;
					gap[IRQPROF_USB_CH4_GAP_SAMPLE]++;
					if (delta > gap[IRQPROF_USB_CH4_GAP_MAX_CYCLES]) {
						gap[IRQPROF_USB_CH4_GAP_MAX_CYCLES] = delta;
					}
					if (cycles_per_us == 0U ||
					    delta <= cycles_per_us * 2U) {
						gap[IRQPROF_USB_CH4_GAP_LE_2US]++;
					} else if (delta <= cycles_per_us * 5U) {
						gap[IRQPROF_USB_CH4_GAP_LE_5US]++;
					} else if (delta <= cycles_per_us * 10U) {
						gap[IRQPROF_USB_CH4_GAP_LE_10US]++;
					} else if (delta <= cycles_per_us * 20U) {
						gap[IRQPROF_USB_CH4_GAP_LE_20US]++;
					} else if (delta <= cycles_per_us * 50U) {
						gap[IRQPROF_USB_CH4_GAP_LE_50US]++;
					} else if (delta <= cycles_per_us * 100U) {
						gap[IRQPROF_USB_CH4_GAP_LE_100US]++;
					} else {
						gap[IRQPROF_USB_CH4_GAP_GT_100US]++;
					}
				}
			}
#endif
			if (channel == 3U && (reasons & (1U << 5)) != 0U) {
				uint32_t hctsiz = *(volatile uint32_t *)(uintptr_t)
					(hcint_addr + 8U);

				irqprof_usb_ch3_ack_counts[bank][IRQPROF_USB_CH3_ACK]++;
				if ((hctsiz & (1UL << 31)) != 0U) {
					irqprof_usb_ch3_ack_counts[bank]
						[IRQPROF_USB_CH3_ACK_DOPING]++;
				}
			}
			for (reason = 0U; reason < IRQPROF_USB_HC_REASON_COUNT;
			     ++reason) {
				if ((reasons & (1U << reason)) != 0U) {
					irqprof_usb_hc_reason_counts[bank][reason]++;
					irqprof_usb_channel_reason_counts[bank]
						[channel][reason]++;
				}
			}
		}
	}
	if ((pending & IRQPROF_USB_PORT_MASK) != 0U) {
		counts[IRQPROF_USB_PORT]++;
		categories++;
	}
	if ((pending & ~IRQPROF_USB_KNOWN_MASK) != 0U) {
		counts[IRQPROF_USB_OTHER]++;
		categories++;
	}
	if (pending == IRQPROF_USB_SOF_MASK) {
		counts[IRQPROF_USB_SOF_ONLY]++;
	}
	if (pending == IRQPROF_USB_HCINT_MASK) {
		counts[IRQPROF_USB_HCINT_ONLY]++;
	}
	if (categories > 1U) {
		counts[IRQPROF_USB_MULTI]++;
	}
}
#endif /* CONFIG_IRQ_PROFILE_USB_CAUSE */

#if CONFIG_USB_IRQ_CH3_ACK_FASTPATH || CONFIG_USB_IRQ_CH4_NAK_COALESCE
extern uint32_t __real_usb_hal_read_interrupts(void);

#if CONFIG_USB_IRQ_CH4_NAK_COALESCE
static void irqprof_usb_ch4_restore_nak_mask(uint32_t hcint_addr)
{
	volatile uint32_t *hcintmsk = (volatile uint32_t *)(uintptr_t)
		(hcint_addr + IRQPROF_USB_HCINTMSK_OFF);

	*hcintmsk |= 1U << 4;
	__DSB();
}
#endif

/*
 * Filter only causes whose complete controller state is unambiguous:
 *
 * - A plain channel-3 non-PING ACK is cleared because the closed HCD's normal
 *   ACK branch is a no-op.
 * - A plain channel-4 NAK temporarily disables its own mask and does not wake
 *   the task.  Hardware then auto-halts the bulk-IN channel and clears raw
 *   NAK.  The following CHHLTD restores the NAK mask and lets the closed HCD's
 *   normal CHHLTD-only bulk-IN path finish and resubmit the transfer.
 *
 * Compound causes and timeout recovery always fall through to the real helper.
 */
uint32_t __attribute__((noinline, used,
	section(".itcm.text.usb_irq_filter")))
__wrap_usb_hal_read_interrupts(void)
{
	uint32_t in_usb_irq =
		__get_IPSR() == (16U + IRQPROF_USB_IRQ);

#if CONFIG_USB_IRQ_CH4_NAK_COALESCE
	{
		uint32_t hcint_addr = IRQPROF_USB_HCINT_BASE +
			4U * IRQPROF_USB_HCINT_STRIDE;
		uint32_t raw = *(volatile uint32_t *)(uintptr_t)hcint_addr;
		volatile uint32_t *coalesce =
			irqprof_usb_ch4_coalesce_counts[irqprof_active_bank];

		if (irqprof_usb_ch4_coalesce_pending != 0U) {
			uint32_t cycles_per_us = SystemCoreClock / 1000000U;
			uint32_t timeout_cycles = cycles_per_us *
				USB_IRQ_CH4_NAK_COALESCE_TIMEOUT_US;
			uint32_t elapsed = DWT->CYCCNT -
				irqprof_usb_ch4_coalesce_start_cycles;

			if ((raw & (1U << 1)) != 0U) {
				irqprof_usb_ch4_restore_nak_mask(hcint_addr);
				if ((raw & (1U << 4)) == 0U) {
					coalesce[
						IRQPROF_USB_CH4_COALESCE_HALT_CLEARED_NAK]++;
				}
				coalesce[IRQPROF_USB_CH4_COALESCE_MERGE]++;
				irqprof_usb_ch4_coalesce_pending = 0U;
				return __real_usb_hal_read_interrupts();
			} else if (cycles_per_us != 0U &&
				   elapsed >= timeout_cycles) {
				irqprof_usb_ch4_restore_nak_mask(hcint_addr);
				coalesce[IRQPROF_USB_CH4_COALESCE_TIMEOUT]++;
				irqprof_usb_ch4_coalesce_pending = 0U;
				return __real_usb_hal_read_interrupts();
			}
		}

		if (in_usb_irq && irqprof_usb_ch4_coalesce_pending == 0U) {
			uint32_t global_pending =
				*(volatile uint32_t *)(uintptr_t)
					IRQPROF_USB_GINTSTS_ADDR &
				*(volatile uint32_t *)(uintptr_t)
					IRQPROF_USB_GINTMSK_ADDR;
			uint32_t channels =
				*(volatile uint32_t *)(uintptr_t)
					IRQPROF_USB_HAINT_ADDR &
				*(volatile uint32_t *)(uintptr_t)
					IRQPROF_USB_HAINTMSK_ADDR &
				0xFFFFU;
			uint32_t reasons = raw &
				*(volatile uint32_t *)(uintptr_t)
					(hcint_addr + IRQPROF_USB_HCINTMSK_OFF);

			if (global_pending == IRQPROF_USB_HCINT_MASK &&
			    channels == (1U << 4) && reasons == (1U << 4)) {
				volatile uint32_t *hcintmsk =
					(volatile uint32_t *)(uintptr_t)
						(hcint_addr + IRQPROF_USB_HCINTMSK_OFF);

				*hcintmsk &= ~(1U << 4);
				irqprof_usb_ch4_coalesce_start_cycles = DWT->CYCCNT;
				irqprof_usb_ch4_coalesce_pending = 1U;
				coalesce[IRQPROF_USB_CH4_COALESCE_DEFER]++;
				__DSB();
				return 0U;
			}
		}
	}
#endif

#if CONFIG_USB_IRQ_CH3_ACK_FASTPATH
	if (in_usb_irq) {
		uint32_t channels =
			*(volatile uint32_t *)(uintptr_t)IRQPROF_USB_HAINT_ADDR &
			*(volatile uint32_t *)(uintptr_t)IRQPROF_USB_HAINTMSK_ADDR &
			0xFFFFU;

		if (channels == (1U << 3)) {
			uint32_t hcint_addr = IRQPROF_USB_HCINT_BASE +
				3U * IRQPROF_USB_HCINT_STRIDE;
			uint32_t reasons =
				*(volatile uint32_t *)(uintptr_t)hcint_addr &
				*(volatile uint32_t *)(uintptr_t)
					(hcint_addr + IRQPROF_USB_HCINTMSK_OFF);
			uint32_t hctsiz = *(volatile uint32_t *)(uintptr_t)
				(hcint_addr + 8U);

			if (reasons == (1U << 5) &&
			    (hctsiz & (1UL << 31)) == 0U) {
				*(volatile uint32_t *)(uintptr_t)hcint_addr = 1U << 5;
#if CONFIG_IRQ_PROFILE_USB_CAUSE
				irqprof_usb_ch3_ack_counts[irqprof_active_bank]
					[IRQPROF_USB_CH3_ACK_FAST_DROP]++;
#endif
				__DSB();
			}
		}
	}
#endif
	return __real_usb_hal_read_interrupts();
}
#endif

#if CONFIG_USB_IRQ_SAFE_DEDUP || CONFIG_USB_IRQ_CLEAR_STALE_PENDING
extern void __real_usb_hal_enable_interrupt(void);

/*
 * The HCD top half leaves USB_IRQn disabled after giving its semaphore.  Just
 * before the HCD task re-enables it, discard the already-latched duplicate and
 * then re-sample the controller.  Re-pending a newly visible cause closes the
 * race which made the earlier single-check experiment lose USB progress.
 */
void __wrap_usb_hal_enable_interrupt(void)
{
#if CONFIG_IRQ_PROFILE_USB_CAUSE && CONFIG_USB_IRQ_CLEAR_STALE_PENDING
	uint32_t bank = irqprof_active_bank;
#endif
#if CONFIG_USB_IRQ_CLEAR_STALE_PENDING
	uint32_t pending =
		*(volatile uint32_t *)(uintptr_t)IRQPROF_USB_GINTSTS_ADDR &
		*(volatile uint32_t *)(uintptr_t)IRQPROF_USB_GINTMSK_ADDR;
#endif

#if CONFIG_IRQ_PROFILE_USB_CAUSE && CONFIG_USB_IRQ_CLEAR_STALE_PENDING
	irqprof_usb_reenable_counts[bank][IRQPROF_USB_REENABLE_CALL]++;
#endif
#if CONFIG_USB_IRQ_SAFE_DEDUP
	if (__get_IPSR() == 0U &&
	    irqprof_usb_handoff_reenable_expected != 0U) {
		uint32_t dedup_bank = irqprof_active_bank;
		uint32_t controller_pending;

		irqprof_usb_handoff_reenable_expected = 0U;
		irqprof_usb_dedup_counts[dedup_bank][IRQPROF_USB_DEDUP_CALL]++;
		if (hal_irq_get_pending((int32_t)IRQPROF_USB_IRQ) != 0U) {
			irqprof_usb_dedup_counts[dedup_bank]
				[IRQPROF_USB_DEDUP_NVIC_PENDING]++;
		}
		hal_irq_clear_pending((int32_t)IRQPROF_USB_IRQ);
		__DSB();
		controller_pending =
			*(volatile uint32_t *)(uintptr_t)IRQPROF_USB_GINTSTS_ADDR &
			*(volatile uint32_t *)(uintptr_t)IRQPROF_USB_GINTMSK_ADDR;
		if (controller_pending != 0U) {
			irqprof_usb_dedup_counts[dedup_bank]
				[IRQPROF_USB_DEDUP_CONTROLLER_PENDING]++;
			hal_irq_set_pending((int32_t)IRQPROF_USB_IRQ);
			__DSB();
			irqprof_usb_dedup_counts[dedup_bank]
				[IRQPROF_USB_DEDUP_REPEND]++;
		}
#if CONFIG_IRQ_PROFILE_USB_CAUSE && CONFIG_IRQ_PROFILE_USB_HANDOFF
	#if CONFIG_IRQ_PROFILE_USB_CH4_FLOW
	if (__get_IPSR() == 0U && irqprof_usb_ch4_service_valid != 0U) {
		volatile uint32_t *flow =
			irqprof_usb_ch4_flow_counts[irqprof_active_bank];
		uint32_t delta = DWT->CYCCNT - irqprof_usb_ch4_service_start_cycles;
		uint32_t reasons = irqprof_usb_ch4_service_reasons;
		uint32_t count_index;
		uint32_t cycles_index;
		uint32_t max_index;

		irqprof_usb_ch4_service_valid = 0U;
		if (reasons == (1U << 4)) {
			count_index = IRQPROF_USB_CH4_NAK_SERVICE;
			cycles_index = IRQPROF_USB_CH4_NAK_SERVICE_CYCLES;
			max_index = IRQPROF_USB_CH4_NAK_SERVICE_MAX_CYCLES;
		} else if (reasons == (1U << 1)) {
			count_index = IRQPROF_USB_CH4_HALT_SERVICE;
			cycles_index = IRQPROF_USB_CH4_HALT_SERVICE_CYCLES;
			max_index = IRQPROF_USB_CH4_HALT_SERVICE_MAX_CYCLES;
		} else {
			flow[IRQPROF_USB_CH4_OTHER_SERVICE]++;
			goto ch4_service_done;
		}
		flow[count_index]++;
		flow[cycles_index] += delta;
		if (delta > flow[max_index]) {
			flow[max_index] = delta;
		}
	}
ch4_service_done:
	#endif
		/* A ch4 NAK is handled by the HCD task by halting the channel.  Time
		 * how soon the resulting CHHLTD becomes visible after IRQ re-enable;
		 * this determines whether a short bounded coalescing window is viable. */
		if ((irqprof_usb_current_channels & (1U << 4)) != 0U &&
		    (irqprof_usb_current_ch4_reasons & (1U << 4)) != 0U &&
		    (irqprof_usb_current_ch4_reasons & (1U << 1)) == 0U) {
			irqprof_usb_ch4_reenable_cycles = DWT->CYCCNT;
			irqprof_usb_ch4_reenable_valid = 1U;
		}
#endif
	}
#endif
#if CONFIG_USB_IRQ_CLEAR_STALE_PENDING
	if (pending == 0U) {
		hal_irq_clear_pending((int32_t)IRQPROF_USB_IRQ);
#if CONFIG_IRQ_PROFILE_USB_CAUSE
		irqprof_usb_reenable_counts[bank][IRQPROF_USB_REENABLE_CLEAR]++;
#endif
	} else {
#if CONFIG_IRQ_PROFILE_USB_CAUSE
		irqprof_usb_reenable_counts[bank][IRQPROF_USB_REENABLE_PENDING]++;
#endif
	}
	__DMB();
#endif
	__real_usb_hal_enable_interrupt();
}
#endif

#if CONFIG_IRQ_PROFILE_USB_CH4_NCM
extern int __real_usbh_ctrl_request(void *host, const void *setup,
				    uint8_t *data);
extern uint32_t __real_ncm_receive_buf_size(void);

int __wrap_usbh_ctrl_request(void *host, const void *setup, uint8_t *data)
{
	const uint8_t *request = (const uint8_t *)setup;
	uint8_t request_type = request != NULL ? request[0] : 0U;
	uint8_t request_code = request != NULL ? request[1] : 0U;
	int result;

	/* CDC-NCM GET_NTB_PARAMETERS and SET_NTB_INPUT_SIZE. */
	if (request_type == 0x21U && request_code == 0x86U && data != NULL) {
		irqprof_usb_ch4_ncm.set_calls++;
		irqprof_usb_ch4_ncm.set_requested = irqprof_get_le32(data);
	}

	result = __real_usbh_ctrl_request(host, setup, data);
	carbox_usb_boot_profiler_record_ctrl(setup, result);

	if (request_type == 0xA1U && request_code == 0x80U) {
		irqprof_usb_ch4_ncm.get_calls++;
		if (result == 0 && data != NULL) {
			irqprof_usb_ch4_ncm.get_ok++;
			irqprof_usb_ch4_ncm.device_in_max =
				irqprof_get_le32(data + 4U);
			irqprof_usb_ch4_ncm.device_out_max =
				irqprof_get_le32(data + 16U);
		} else {
			irqprof_usb_ch4_ncm.get_fail++;
		}
	} else if (request_type == 0x21U && request_code == 0x86U) {
		if (result == 0) {
			irqprof_usb_ch4_ncm.set_ok++;
		} else {
			irqprof_usb_ch4_ncm.set_fail++;
		}
	}

	return result;
}

uint32_t __wrap_ncm_receive_buf_size(void)
{
	uint32_t size = __real_ncm_receive_buf_size();

	irqprof_usb_ch4_ncm.receive_size = size;
	irqprof_usb_ch4_ncm.receive_size_calls++;
	return size;
}
#endif

#if CONFIG_IRQ_PROFILE_USB_CH4_FLOW
extern void __real_usbh_hal_hc_halt(uint8_t channel);

void __wrap_usbh_hal_hc_halt(uint8_t channel)
{
	if (channel == 4U && __get_IPSR() == 0U &&
	    irqprof_usb_ch4_service_valid != 0U) {
		volatile uint32_t *flow =
			irqprof_usb_ch4_flow_counts[irqprof_active_bank];

		if (irqprof_usb_ch4_service_reasons == (1U << 4)) {
			flow[IRQPROF_USB_CH4_NAK_HALT_CALL]++;
		} else {
			flow[IRQPROF_USB_CH4_OTHER_HALT_CALL]++;
		}
	}
	__real_usbh_hal_hc_halt(channel);
}
#endif

#if CONFIG_IRQ_PROFILE_USB_HANDOFF || CONFIG_USB_IRQ_SAFE_DEDUP
void carbox_irq_profiler_usb_sema_give(void *handle, int success,
				       int higher_priority_task_woken)
{
	uint32_t bank;

	/* External IRQ 12 has exception number 16 + 12. */
	if (__get_IPSR() != (16U + IRQPROF_USB_IRQ)) {
		return;
	}
	bank = irqprof_active_bank;
#if CONFIG_IRQ_PROFILE_USB_HANDOFF
	irqprof_usb_handoff_counts[bank][IRQPROF_USB_HANDOFF_GIVE]++;
#endif
	if (!success) {
#if CONFIG_IRQ_PROFILE_USB_HANDOFF
		irqprof_usb_handoff_counts[bank][IRQPROF_USB_HANDOFF_GIVE_FAIL]++;
#endif
		return;
	}

#if CONFIG_IRQ_PROFILE_USB_HANDOFF
	irqprof_usb_handoff_counts[bank][IRQPROF_USB_HANDOFF_GIVE_OK]++;
	{
		uint32_t channel;
		uint32_t channels = irqprof_usb_current_channels;

		/* NCM currently occupies channels 2..4.  Keeping this narrow avoids
		 * turning the diagnostic itself into measurable ISR work. */
		for (channel = 2U; channel <= 4U; ++channel) {
			if ((channels & (1U << channel)) == 0U) {
				continue;
			}
			irqprof_usb_channel_handoff_counts[bank][channel]
				[IRQPROF_USB_CHANNEL_HANDOFF_GIVE]++;
			if (higher_priority_task_woken) {
				irqprof_usb_channel_handoff_counts[bank][channel]
					[IRQPROF_USB_CHANNEL_HANDOFF_YIELD]++;
			}
		}
#if CONFIG_IRQ_PROFILE_USB_CH4_FLOW
		if ((channels & (1U << 4)) != 0U) {
			irqprof_usb_ch4_service_start_cycles = DWT->CYCCNT;
			irqprof_usb_ch4_service_reasons =
				irqprof_usb_current_ch4_reasons;
			irqprof_usb_ch4_service_valid = 1U;
		}
#endif
	}
	if (higher_priority_task_woken) {
		irqprof_usb_handoff_counts[bank][IRQPROF_USB_HANDOFF_YIELD]++;
	} else {
		irqprof_usb_handoff_counts[bank][IRQPROF_USB_HANDOFF_NO_YIELD]++;
	}
#else
	(void)bank;
	(void)higher_priority_task_woken;
#endif
#if CONFIG_USB_IRQ_SAFE_DEDUP
	irqprof_usb_handoff_reenable_expected = 1U;
#endif
	(void)handle;
}
#else
void carbox_irq_profiler_usb_sema_give(void *handle, int success,
				       int higher_priority_task_woken)
{
	(void)handle;
	(void)success;
	(void)higher_priority_task_woken;
}
#endif
static hal_irq_api_t irqprof_original_api;
static hal_irq_api_t irqprof_hook_api;
static uint32_t irqprof_initialized;
static uint32_t irqprof_hook_ok;
static uint32_t irqprof_api_hook_visible;
static uint32_t irqprof_initial_vectors;
static uint32_t irqprof_repairs;
static uint32_t irqprof_snapshot_repairs;
static uint32_t irqprof_snapshot_profiler_irq = UINT32_MAX;
static uint32_t irqprof_snapshot_profiler_callbacks;
static uint32_t irqprof_snapshot_profiler_accounted;

/*
 * All peripheral vectors point here.  IPSR identifies the current vector, so
 * one trampoline covers all 32 IRQs.  It deliberately uses only r0-r3, which
 * Cortex-M hardware already saved on exception entry, and never touches SP.
 * The final BX is a tail branch: the vendor handler receives the original
 * EXC_RETURN in LR exactly as if it were installed directly in the vector
 * table.  There is no C call frame, lock, semaphore, or profiling print in the
 * interrupt path.
 */
static void __attribute__((naked, used, section(".itcm.text.irqprof_dispatch")))
irqprof_dispatch(void)
{
	__asm volatile(
		"mrs r0, ipsr\n"
		"subs r0, r0, #16\n"
		"cmp r0, #31\n"
		"bhi 2f\n"
		"ldr r1, =irqprof_active_bank\n"
		"ldr r2, [r1]\n"
		"ldr r1, =irqprof_counts\n"
		"add.w r1, r1, r2, lsl #7\n"
		"ldr.w r2, [r1, r0, lsl #2]\n"
		"adds r2, r2, #1\n"
		"str.w r2, [r1, r0, lsl #2]\n"
#if CONFIG_IRQ_PROFILE_USB_CAUSE
		"cmp r0, #12\n"
		"bne 1f\n"
		/* Preserve the IRQ index and EXC_RETURN.  Eight bytes keeps the
		 * exception stack ABI aligned for the normal C helper. */
		"push {r0, lr}\n"
		"bl irqprof_usb_cause_sample\n"
		"pop {r0, lr}\n"
		"1:\n"
#endif
		"ldr r1, =irqprof_handlers\n"
		"ldr.w r1, [r1, r0, lsl #2]\n"
		"cbz r1, 2f\n"
		"bx r1\n"
		"2: bx lr\n");
}

static void irqprof_set_vector(int32_t irqn, uint32_t vector)
{
	uint32_t dispatch = (uint32_t)(uintptr_t)irqprof_dispatch;

	if (irqn < 0 || irqn >= (int32_t)IRQPROF_IRQ_COUNT ||
	    irqprof_original_api.irq_set_vector == NULL) {
		if (irqprof_original_api.irq_set_vector != NULL) {
			irqprof_original_api.irq_set_vector(irqn, vector);
		}
		return;
	}

	/* An internal refresh may reinstall the trampoline; do not replace the
	 * saved vendor handler with the trampoline itself. */
	if ((vector & ~1U) != (dispatch & ~1U)) {
		irqprof_handlers[(uint32_t)irqn] = vector;
		__DMB();
	}
	irqprof_original_api.irq_set_vector(
		irqn, vector != 0U ? dispatch : 0U);
}

/* Must be called with peripheral interrupts masked. */
static uint32_t irqprof_audit_vectors(void)
{
	int_vector_t *vectors = hal_int_vector_stubs.ram_vector_table;
	uint32_t dispatch = (uint32_t)(uintptr_t)irqprof_dispatch;
	uint32_t repaired = 0U;
	uint32_t i;

	if (!irqprof_hook_ok || vectors == NULL ||
	    irqprof_original_api.irq_set_vector == NULL) {
		return 0U;
	}

	for (i = 0U; i < IRQPROF_IRQ_COUNT; ++i) {
		uint32_t vector = (uint32_t)(uintptr_t)vectors[16U + i];

		if (vector == 0U) {
			irqprof_handlers[i] = 0U;
			continue;
		}
		if ((vector & ~1U) == (dispatch & ~1U)) {
			continue;
		}

		/* Catch code which bypassed the HAL API table and wrote VTOR directly. */
		irqprof_handlers[i] = vector;
		__DMB();
		irqprof_original_api.irq_set_vector((int32_t)i, dispatch);
		repaired++;
	}
	return repaired;
}

void carbox_irq_profiler_init(void)
{
	hal_irq_api_t *current;
	uint32_t primask;
	uint32_t i;

	if (irqprof_initialized) {
		return;
	}
	irqprof_initialized = 1U;
	current = hal_int_vector_stubs.pirq_api_tbl;
	if (current == NULL || current->irq_set_vector == NULL ||
	    current->irq_get_vector == NULL ||
	    hal_int_vector_stubs.hal_irq_api_init == NULL) {
		rt_printf("[IRQPROF] ERROR: HAL IRQ API table unavailable\r\n");
		return;
	}

	irqprof_original_api = *current;
	irqprof_hook_api = *current;
	irqprof_hook_api.irq_set_vector = irqprof_set_vector;

	primask = __get_PRIMASK();
	__disable_irq();
	/* Preserve handlers registered before main(), then install the supported
	 * HAL API hook so every later registration is wrapped automatically. */
	for (i = 0U; i < IRQPROF_IRQ_COUNT; ++i) {
		irqprof_handlers[i] = irqprof_original_api.irq_get_vector((int32_t)i);
	}
	hal_int_vector_stubs.hal_irq_api_init(&irqprof_hook_api);
	current = hal_int_vector_stubs.pirq_api_tbl;
	/* pirq_api_tbl is the ROM table exported for inspection; on this SDK it
	 * need not change when hal_irq_api_init() updates the ROM-internal hook.
	 * Therefore visibility is diagnostic only. Direct vector auditing remains
	 * a complete fallback and repairs registrations which bypass that hook. */
	irqprof_api_hook_visible = current != NULL &&
		current->irq_set_vector == irqprof_set_vector;
	irqprof_hook_ok = 1U;
	irqprof_initial_vectors = irqprof_audit_vectors();
	if (primask == 0U) {
		__enable_irq();
	}

	#if CONFIG_IRQ_PROFILE_REPORT
	rt_printf("[IRQPROF] enabled scope=external irq=0..31 hook=HAL-api-table "
		  "api_visible=%lu initial_vectors=%lu "
		  "fallback=periodic-vector-audit hotpath=naked-counter-tailbranch\r\n",
		  (unsigned long)irqprof_api_hook_visible,
		  (unsigned long)irqprof_initial_vectors);
	#endif
}

void carbox_irq_profiler_snapshot(uint32_t profiler_irq,
				  uint32_t profiler_callbacks)
{
	uint32_t primask;
	uint32_t old_bank;
	uint32_t new_bank;
	uint32_t i;

	if (!irqprof_hook_ok) {
		return;
	}

	/* This is normally called from inside the PC profiler's existing PRIMASK
	 * snapshot.  Retaining the local save/restore makes the API safe if it is
	 * reused independently without ever enabling interrupts unexpectedly. */
	primask = __get_PRIMASK();
	__disable_irq();
	irqprof_snapshot_repairs = irqprof_audit_vectors();
	irqprof_repairs += irqprof_snapshot_repairs;
	old_bank = irqprof_active_bank;
	new_bank = old_bank ^ 1U;
	memset((void *)irqprof_counts[new_bank], 0,
	       sizeof(irqprof_counts[new_bank]));
#if CONFIG_IRQ_PROFILE_USB_HANDOFF
	memset((void *)irqprof_usb_handoff_counts[new_bank], 0,
	       sizeof(irqprof_usb_handoff_counts[new_bank]));
#if CONFIG_IRQ_PROFILE_USB_CAUSE
	memset((void *)irqprof_usb_channel_handoff_counts[new_bank], 0,
	       sizeof(irqprof_usb_channel_handoff_counts[new_bank]));
	memset((void *)irqprof_usb_ch4_halt_gap_counts[new_bank], 0,
	       sizeof(irqprof_usb_ch4_halt_gap_counts[new_bank]));
#if CONFIG_IRQ_PROFILE_USB_CH4_FLOW
	memset((void *)irqprof_usb_ch4_flow_counts[new_bank], 0,
	       sizeof(irqprof_usb_ch4_flow_counts[new_bank]));
#endif
#endif
#endif
#if CONFIG_USB_IRQ_SAFE_DEDUP
	memset((void *)irqprof_usb_dedup_counts[new_bank], 0,
	       sizeof(irqprof_usb_dedup_counts[new_bank]));
#endif
#if CONFIG_USB_IRQ_CH4_NAK_COALESCE
	memset((void *)irqprof_usb_ch4_coalesce_counts[new_bank], 0,
	       sizeof(irqprof_usb_ch4_coalesce_counts[new_bank]));
#endif
#if CONFIG_IRQ_PROFILE_USB_CAUSE
	memset((void *)irqprof_usb_counts[new_bank], 0,
	       sizeof(irqprof_usb_counts[new_bank]));
	memset((void *)irqprof_usb_channel_counts[new_bank], 0,
	       sizeof(irqprof_usb_channel_counts[new_bank]));
	memset((void *)irqprof_usb_hc_reason_counts[new_bank], 0,
	       sizeof(irqprof_usb_hc_reason_counts[new_bank]));
	memset((void *)irqprof_usb_channel_reason_counts[new_bank], 0,
	       sizeof(irqprof_usb_channel_reason_counts[new_bank]));
	memset((void *)irqprof_usb_channel_hcchar[new_bank], 0,
	       sizeof(irqprof_usb_channel_hcchar[new_bank]));
	memset((void *)irqprof_usb_ch3_ack_counts[new_bank], 0,
	       sizeof(irqprof_usb_ch3_ack_counts[new_bank]));
#if CONFIG_IRQ_PROFILE_USB_CH4_SEQUENCE
	memset((void *)irqprof_usb_ch4_sequence_counts[new_bank], 0,
	       sizeof(irqprof_usb_ch4_sequence_counts[new_bank]));
#endif
#if CONFIG_USB_IRQ_CLEAR_STALE_PENDING
	memset((void *)irqprof_usb_reenable_counts[new_bank], 0,
	       sizeof(irqprof_usb_reenable_counts[new_bank]));
#endif
#endif
	irqprof_active_bank = new_bank;
	__DMB();
	for (i = 0U; i < IRQPROF_IRQ_COUNT; ++i) {
		irqprof_snapshot[i] = irqprof_counts[old_bank][i];
	}
#if CONFIG_IRQ_PROFILE_USB_HANDOFF
	for (i = 0U; i < IRQPROF_USB_HANDOFF_COUNTER_COUNT; ++i) {
		irqprof_usb_handoff_snapshot[i] =
			irqprof_usb_handoff_counts[old_bank][i];
	}
#if CONFIG_IRQ_PROFILE_USB_CAUSE
	for (i = 0U; i < IRQPROF_USB_CHANNEL_COUNT; ++i) {
		memcpy(irqprof_usb_channel_handoff_snapshot[i],
		       (const void *)irqprof_usb_channel_handoff_counts[old_bank][i],
		       sizeof(irqprof_usb_channel_handoff_snapshot[i]));
	}
	for (i = 0U; i < IRQPROF_USB_CH4_GAP_COUNTER_COUNT; ++i) {
		irqprof_usb_ch4_halt_gap_snapshot[i] =
			irqprof_usb_ch4_halt_gap_counts[old_bank][i];
	}
#if CONFIG_IRQ_PROFILE_USB_CH4_FLOW
	for (i = 0U; i < IRQPROF_USB_CH4_FLOW_COUNTER_COUNT; ++i) {
		irqprof_usb_ch4_flow_snapshot[i] =
			irqprof_usb_ch4_flow_counts[old_bank][i];
	}
#endif
#endif
#endif
#if CONFIG_USB_IRQ_SAFE_DEDUP
	for (i = 0U; i < IRQPROF_USB_DEDUP_COUNTER_COUNT; ++i) {
		irqprof_usb_dedup_snapshot[i] =
			irqprof_usb_dedup_counts[old_bank][i];
	}
#endif
#if CONFIG_USB_IRQ_CH4_NAK_COALESCE
	for (i = 0U; i < IRQPROF_USB_CH4_COALESCE_COUNTER_COUNT; ++i) {
		irqprof_usb_ch4_coalesce_snapshot[i] =
			irqprof_usb_ch4_coalesce_counts[old_bank][i];
	}
#endif
#if CONFIG_IRQ_PROFILE_USB_CAUSE
	for (i = 0U; i < IRQPROF_USB_COUNTER_COUNT; ++i) {
		irqprof_usb_snapshot[i] = irqprof_usb_counts[old_bank][i];
	}
	for (i = 0U; i < IRQPROF_USB_CHANNEL_COUNT; ++i) {
		irqprof_usb_channel_snapshot[i] =
			irqprof_usb_channel_counts[old_bank][i];
	}
	for (i = 0U; i < IRQPROF_USB_HC_REASON_COUNT; ++i) {
		irqprof_usb_hc_reason_snapshot[i] =
			irqprof_usb_hc_reason_counts[old_bank][i];
	}
	for (i = 0U; i < IRQPROF_USB_CHANNEL_COUNT; ++i) {
		memcpy(irqprof_usb_channel_reason_snapshot[i],
		       (const void *)irqprof_usb_channel_reason_counts[old_bank][i],
		       sizeof(irqprof_usb_channel_reason_snapshot[i]));
		irqprof_usb_channel_hcchar_snapshot[i] =
			irqprof_usb_channel_hcchar[old_bank][i];
	}
	for (i = 0U; i < IRQPROF_USB_CH3_ACK_COUNTER_COUNT; ++i) {
		irqprof_usb_ch3_ack_snapshot[i] =
			irqprof_usb_ch3_ack_counts[old_bank][i];
	}
#if CONFIG_IRQ_PROFILE_USB_CH4_SEQUENCE
	for (i = 0U; i < IRQPROF_USB_CH4_SEQ_COUNTER_COUNT; ++i) {
		irqprof_usb_ch4_sequence_snapshot[i] =
			irqprof_usb_ch4_sequence_counts[old_bank][i];
	}
#endif
#if CONFIG_USB_IRQ_CLEAR_STALE_PENDING
	for (i = 0U; i < IRQPROF_USB_REENABLE_COUNTER_COUNT; ++i) {
		irqprof_usb_reenable_snapshot[i] =
			irqprof_usb_reenable_counts[old_bank][i];
	}
#endif
#endif
	irqprof_snapshot_profiler_irq = profiler_irq;
	irqprof_snapshot_profiler_callbacks = profiler_callbacks;
	irqprof_snapshot_profiler_accounted = 0U;
	if (profiler_irq < IRQPROF_IRQ_COUNT) {
		uint32_t group_irqs = irqprof_snapshot[profiler_irq];

		irqprof_snapshot_profiler_accounted =
			profiler_callbacks < group_irqs ? profiler_callbacks : group_irqs;
	}
	if (primask == 0U) {
		__enable_irq();
	}
}

static uint32_t irqprof_adjusted_count(uint32_t irq)
{
	uint32_t count = irqprof_snapshot[irq];

	if (irq == irqprof_snapshot_profiler_irq) {
		count -= irqprof_snapshot_profiler_accounted;
	}
	return count;
}

void carbox_irq_profiler_report(uint32_t sequence, uint32_t window_ms)
{
	uint8_t selected[IRQPROF_IRQ_COUNT] = { 0U };
	uint32_t top[IRQPROF_TOP_COUNT];
	uint32_t raw_total = 0U;
	uint32_t adjusted_total = 0U;
	uint32_t raw_active = 0U;
	uint32_t adjusted_active = 0U;
	uint32_t printed = 0U;
	uint32_t top_count = 0U;
	uint32_t i;

	if (!irqprof_hook_ok || window_ms == 0U) {
		return;
	}

	for (i = 0U; i < IRQPROF_IRQ_COUNT; ++i) {
		uint32_t adjusted = irqprof_adjusted_count(i);

		raw_total += irqprof_snapshot[i];
		adjusted_total += adjusted;
		if (irqprof_snapshot[i] != 0U) {
			raw_active++;
		}
		if (adjusted != 0U) {
			adjusted_active++;
		}
	}
	for (top_count = 0U; top_count < IRQPROF_TOP_COUNT; ++top_count) {
		uint32_t best = IRQPROF_IRQ_COUNT;

		for (i = 0U; i < IRQPROF_IRQ_COUNT; ++i) {
			if (!selected[i] && irqprof_adjusted_count(i) != 0U &&
			    (best == IRQPROF_IRQ_COUNT ||
			     irqprof_adjusted_count(i) >
				irqprof_adjusted_count(best))) {
				best = i;
			}
		}
		if (best == IRQPROF_IRQ_COUNT) {
			break;
		}
		selected[best] = 1U;
		top[top_count] = best;
	}

	rt_printf("[IRQPROF][%lu] window_ms=%lu scope=external "
		  "raw/adjusted=%lu/%lu adjusted_rate=%lu/s "
		  "active raw/adjusted=%lu/%lu top=%lu repairs_now/total=%lu/%lu "
		  "initial_vectors=%lu adjustment=pc-timer-only "
		  "uart_includes_profiler_log=1 excludes=SysTick/PendSV/SVC\r\n",
		  (unsigned long)sequence, (unsigned long)window_ms,
		  (unsigned long)raw_total, (unsigned long)adjusted_total,
		  (unsigned long)(((uint64_t)adjusted_total * 1000U) / window_ms),
		  (unsigned long)raw_active, (unsigned long)adjusted_active,
		  (unsigned long)top_count,
		  (unsigned long)irqprof_snapshot_repairs,
		  (unsigned long)irqprof_repairs,
		  (unsigned long)irqprof_initial_vectors);
	if (irqprof_snapshot_profiler_irq < IRQPROF_IRQ_COUNT) {
		uint32_t irq = irqprof_snapshot_profiler_irq;
		uint32_t raw = irqprof_snapshot[irq];
		uint32_t unmatched = irqprof_snapshot_profiler_callbacks -
			irqprof_snapshot_profiler_accounted;

		rt_printf("[IRQPROF][%lu][SELF] irq=%lu name=%s group_raw=%lu "
			  "callbacks/deducted/unmatched=%lu/%lu/%lu other_est=%lu\r\n",
			  (unsigned long)sequence, (unsigned long)irq,
			  irqprof_names[irq], (unsigned long)raw,
			  (unsigned long)irqprof_snapshot_profiler_callbacks,
			  (unsigned long)irqprof_snapshot_profiler_accounted,
			  (unsigned long)unmatched,
			  (unsigned long)(raw -
				irqprof_snapshot_profiler_accounted));
	}

#if CONFIG_USB_IRQ_CH4_NAK_COALESCE
	rt_printf("[IRQPROF][%lu][USB_CH4_COALESCE] "
		  "defer/merge/timeout/halt_cleared_nak=%lu/%lu/%lu/%lu "
		  "pending=%lu saved_handoff_rate=%lu/s\r\n",
		  (unsigned long)sequence,
		  (unsigned long)irqprof_usb_ch4_coalesce_snapshot[
			IRQPROF_USB_CH4_COALESCE_DEFER],
		  (unsigned long)irqprof_usb_ch4_coalesce_snapshot[
			IRQPROF_USB_CH4_COALESCE_MERGE],
		  (unsigned long)irqprof_usb_ch4_coalesce_snapshot[
			IRQPROF_USB_CH4_COALESCE_TIMEOUT],
		  (unsigned long)irqprof_usb_ch4_coalesce_snapshot[
			IRQPROF_USB_CH4_COALESCE_HALT_CLEARED_NAK],
		  (unsigned long)irqprof_usb_ch4_coalesce_pending,
		  (unsigned long)(((uint64_t)irqprof_usb_ch4_coalesce_snapshot[
			IRQPROF_USB_CH4_COALESCE_MERGE] * 1000U) / window_ms));
#endif

#if CONFIG_IRQ_PROFILE_USB_CAUSE
	{
		uint32_t samples = irqprof_usb_snapshot[IRQPROF_USB_SAMPLE];
		uint32_t usb_irqs = irqprof_snapshot[IRQPROF_USB_IRQ];

		rt_printf("[IRQPROF][%lu][USB_CAUSE] samples/usb_irq=%lu/%lu "
			  "zero=%lu sof/only=%lu/%lu hcint/only=%lu/%lu "
			  "port=%lu other=%lu multi=%lu hc_no_channel=%lu "
			  "rate sof/hcint=%lu/%lu/s\r\n",
			  (unsigned long)sequence,
			  (unsigned long)samples, (unsigned long)usb_irqs,
			  (unsigned long)irqprof_usb_snapshot[IRQPROF_USB_ZERO],
			  (unsigned long)irqprof_usb_snapshot[IRQPROF_USB_SOF],
			  (unsigned long)irqprof_usb_snapshot[IRQPROF_USB_SOF_ONLY],
			  (unsigned long)irqprof_usb_snapshot[IRQPROF_USB_HCINT],
			  (unsigned long)irqprof_usb_snapshot[IRQPROF_USB_HCINT_ONLY],
			  (unsigned long)irqprof_usb_snapshot[IRQPROF_USB_PORT],
			  (unsigned long)irqprof_usb_snapshot[IRQPROF_USB_OTHER],
			  (unsigned long)irqprof_usb_snapshot[IRQPROF_USB_MULTI],
			  (unsigned long)
				irqprof_usb_snapshot[IRQPROF_USB_HC_NO_CHANNEL],
			  (unsigned long)(((uint64_t)
				irqprof_usb_snapshot[IRQPROF_USB_SOF] * 1000U) /
				window_ms),
			  (unsigned long)(((uint64_t)
				irqprof_usb_snapshot[IRQPROF_USB_HCINT] * 1000U) /
				window_ms));
#if CONFIG_USB_IRQ_CLEAR_STALE_PENDING
		rt_printf("[IRQPROF][%lu][USB_REENABLE] calls/clear/pending=%lu/%lu/%lu\r\n",
			  (unsigned long)sequence,
			  (unsigned long)irqprof_usb_reenable_snapshot[
				IRQPROF_USB_REENABLE_CALL],
			  (unsigned long)irqprof_usb_reenable_snapshot[
				IRQPROF_USB_REENABLE_CLEAR],
			  (unsigned long)irqprof_usb_reenable_snapshot[
				IRQPROF_USB_REENABLE_PENDING]);
#endif
		rt_printf("[IRQPROF][%lu][USB_HC_REASON] "
			  "xfer/chhalt/ahberr/stall/nak/ack/nyet/xacterr="
			  "%lu/%lu/%lu/%lu/%lu/%lu/%lu/%lu "
			  "babble/frame/toggle/bna/xcs/roll="
			  "%lu/%lu/%lu/%lu/%lu/%lu\r\n",
			  (unsigned long)sequence,
			  (unsigned long)irqprof_usb_hc_reason_snapshot[0],
			  (unsigned long)irqprof_usb_hc_reason_snapshot[1],
			  (unsigned long)irqprof_usb_hc_reason_snapshot[2],
			  (unsigned long)irqprof_usb_hc_reason_snapshot[3],
			  (unsigned long)irqprof_usb_hc_reason_snapshot[4],
			  (unsigned long)irqprof_usb_hc_reason_snapshot[5],
			  (unsigned long)irqprof_usb_hc_reason_snapshot[6],
			  (unsigned long)irqprof_usb_hc_reason_snapshot[7],
			  (unsigned long)irqprof_usb_hc_reason_snapshot[8],
			  (unsigned long)irqprof_usb_hc_reason_snapshot[9],
			  (unsigned long)irqprof_usb_hc_reason_snapshot[10],
			  (unsigned long)irqprof_usb_hc_reason_snapshot[11],
			  (unsigned long)irqprof_usb_hc_reason_snapshot[12],
			  (unsigned long)irqprof_usb_hc_reason_snapshot[13]);
		rt_printf("[IRQPROF][%lu][USB_CH3_ACK] total/doping/fast_drop="
			  "%lu/%lu/%lu doping_pct=%lu.%02lu%%\r\n",
			  (unsigned long)sequence,
			  (unsigned long)irqprof_usb_ch3_ack_snapshot[
				IRQPROF_USB_CH3_ACK],
			  (unsigned long)irqprof_usb_ch3_ack_snapshot[
				IRQPROF_USB_CH3_ACK_DOPING],
			  (unsigned long)irqprof_usb_ch3_ack_snapshot[
				IRQPROF_USB_CH3_ACK_FAST_DROP],
			  (unsigned long)(irqprof_usb_ch3_ack_snapshot[
				IRQPROF_USB_CH3_ACK] != 0U ?
				((uint64_t)irqprof_usb_ch3_ack_snapshot[
					IRQPROF_USB_CH3_ACK_DOPING] * 10000U /
				 irqprof_usb_ch3_ack_snapshot[IRQPROF_USB_CH3_ACK]) /
				100U : 0U),
			  (unsigned long)(irqprof_usb_ch3_ack_snapshot[
				IRQPROF_USB_CH3_ACK] != 0U ?
				((uint64_t)irqprof_usb_ch3_ack_snapshot[
					IRQPROF_USB_CH3_ACK_DOPING] * 10000U /
				 irqprof_usb_ch3_ack_snapshot[IRQPROF_USB_CH3_ACK]) %
				100U : 0U));
		{
			uint8_t channel_selected[IRQPROF_USB_CHANNEL_COUNT] = { 0U };
			uint32_t rank;

			for (rank = 0U; rank < 4U; ++rank) {
				uint32_t best = IRQPROF_USB_CHANNEL_COUNT;
				uint32_t channel;

				for (channel = 0U;
				     channel < IRQPROF_USB_CHANNEL_COUNT; ++channel) {
					if (!channel_selected[channel] &&
					    irqprof_usb_channel_snapshot[channel] != 0U &&
					    (best == IRQPROF_USB_CHANNEL_COUNT ||
					     irqprof_usb_channel_snapshot[channel] >
						irqprof_usb_channel_snapshot[best])) {
						best = channel;
					}
				}
				if (best == IRQPROF_USB_CHANNEL_COUNT) {
					break;
				}
				channel_selected[best] = 1U;
				{
					uint32_t hcchar =
						irqprof_usb_channel_hcchar_snapshot[best];
					uint32_t *reason =
						irqprof_usb_channel_reason_snapshot[best];

					rt_printf("[IRQPROF][%lu][USB_HC] #%lu channel=%lu "
					  "interrupts=%lu rate=%lu/s "
					  "dev/ep/dir/type/mps=%lu/%lu/%s/%lu/%lu "
					  "reason xfer/halt/nak/ack/nyet="
					  "%lu/%lu/%lu/%lu/%lu hcchar=%08lx\r\n",
					  (unsigned long)sequence,
					  (unsigned long)(rank + 1U),
					  (unsigned long)best,
					  (unsigned long)
						irqprof_usb_channel_snapshot[best],
					  (unsigned long)(((uint64_t)
						irqprof_usb_channel_snapshot[best] *
						1000U) / window_ms),
					  (unsigned long)((hcchar >> 22) & 0x7FU),
					  (unsigned long)((hcchar >> 11) & 0x0FU),
					  (hcchar & (1U << 15)) != 0U ? "IN" : "OUT",
					  (unsigned long)((hcchar >> 18) & 0x03U),
					  (unsigned long)(hcchar & 0x7FFU),
					  (unsigned long)reason[0],
					  (unsigned long)reason[1],
					  (unsigned long)reason[4],
					  (unsigned long)reason[5],
					  (unsigned long)reason[6],
					  (unsigned long)hcchar);
				}
			}
		}
	}
#endif

#if CONFIG_IRQ_PROFILE_USB_HANDOFF
	{
		uint32_t give = irqprof_usb_handoff_snapshot[
			IRQPROF_USB_HANDOFF_GIVE];
		uint32_t yield = irqprof_usb_handoff_snapshot[
			IRQPROF_USB_HANDOFF_YIELD];

		rt_printf("[IRQPROF][%lu][USB_HANDOFF] "
			  "give/ok/fail=%lu/%lu/%lu yield/no_yield=%lu/%lu "
			  "yield_pct=%lu.%02lu%% rate give/yield=%lu/%lu/s\r\n",
			  (unsigned long)sequence,
			  (unsigned long)give,
			  (unsigned long)irqprof_usb_handoff_snapshot[
				IRQPROF_USB_HANDOFF_GIVE_OK],
			  (unsigned long)irqprof_usb_handoff_snapshot[
				IRQPROF_USB_HANDOFF_GIVE_FAIL],
			  (unsigned long)yield,
			  (unsigned long)irqprof_usb_handoff_snapshot[
				IRQPROF_USB_HANDOFF_NO_YIELD],
			  (unsigned long)(give != 0U ?
				((uint64_t)yield * 10000U / give) / 100U : 0U),
			  (unsigned long)(give != 0U ?
				((uint64_t)yield * 10000U / give) % 100U : 0U),
			  (unsigned long)(((uint64_t)give * 1000U) / window_ms),
			  (unsigned long)(((uint64_t)yield * 1000U) / window_ms));
	}
#if CONFIG_IRQ_PROFILE_USB_CAUSE
	for (i = 2U; i <= 4U; ++i) {
		uint32_t give = irqprof_usb_channel_handoff_snapshot[i]
			[IRQPROF_USB_CHANNEL_HANDOFF_GIVE];
		uint32_t yield = irqprof_usb_channel_handoff_snapshot[i]
			[IRQPROF_USB_CHANNEL_HANDOFF_YIELD];

		if (give == 0U) {
			continue;
		}
		rt_printf("[IRQPROF][%lu][USB_HANDOFF_HC] "
			  "ch=%lu give/yield/no_yield=%lu/%lu/%lu "
			  "yield_pct=%lu.%02lu%% rate give/yield=%lu/%lu/s\r\n",
			  (unsigned long)sequence, (unsigned long)i,
			  (unsigned long)give, (unsigned long)yield,
			  (unsigned long)(give - yield),
			  (unsigned long)(((uint64_t)yield * 10000U / give) /
					 100U),
			  (unsigned long)(((uint64_t)yield * 10000U / give) %
					 100U),
			  (unsigned long)(((uint64_t)give * 1000U) / window_ms),
			  (unsigned long)(((uint64_t)yield * 1000U) / window_ms));
	}
	{
		uint32_t max_us = SystemCoreClock != 0U ?
			(uint32_t)(((uint64_t)irqprof_usb_ch4_halt_gap_snapshot[
				IRQPROF_USB_CH4_GAP_MAX_CYCLES] * 1000000ULL) /
				SystemCoreClock) : 0U;

		rt_printf("[IRQPROF][%lu][USB_CH4_HALT_GAP] "
			  "samples=%lu bins_us <=2/<=5/<=10/<=20/<=50/<=100/>100="
			  "%lu/%lu/%lu/%lu/%lu/%lu/%lu max_us=%lu\r\n",
			  (unsigned long)sequence,
			  (unsigned long)irqprof_usb_ch4_halt_gap_snapshot[
				IRQPROF_USB_CH4_GAP_SAMPLE],
			  (unsigned long)irqprof_usb_ch4_halt_gap_snapshot[
				IRQPROF_USB_CH4_GAP_LE_2US],
			  (unsigned long)irqprof_usb_ch4_halt_gap_snapshot[
				IRQPROF_USB_CH4_GAP_LE_5US],
			  (unsigned long)irqprof_usb_ch4_halt_gap_snapshot[
				IRQPROF_USB_CH4_GAP_LE_10US],
			  (unsigned long)irqprof_usb_ch4_halt_gap_snapshot[
				IRQPROF_USB_CH4_GAP_LE_20US],
			  (unsigned long)irqprof_usb_ch4_halt_gap_snapshot[
				IRQPROF_USB_CH4_GAP_LE_50US],
			  (unsigned long)irqprof_usb_ch4_halt_gap_snapshot[
				IRQPROF_USB_CH4_GAP_LE_100US],
			  (unsigned long)irqprof_usb_ch4_halt_gap_snapshot[
				IRQPROF_USB_CH4_GAP_GT_100US],
			  (unsigned long)max_us);
	}
#if CONFIG_IRQ_PROFILE_USB_CH4_FLOW
	{
		uint32_t nak = irqprof_usb_ch4_flow_snapshot[
			IRQPROF_USB_CH4_NAK_SERVICE];
		uint32_t halt = irqprof_usb_ch4_flow_snapshot[
			IRQPROF_USB_CH4_HALT_SERVICE];
		uint32_t cycles_per_us = SystemCoreClock / 1000000U;
		uint32_t nak_avg_us = nak != 0U && cycles_per_us != 0U ?
			irqprof_usb_ch4_flow_snapshot[
				IRQPROF_USB_CH4_NAK_SERVICE_CYCLES] /
				nak / cycles_per_us : 0U;
		uint32_t halt_avg_us = halt != 0U && cycles_per_us != 0U ?
			irqprof_usb_ch4_flow_snapshot[
				IRQPROF_USB_CH4_HALT_SERVICE_CYCLES] /
				halt / cycles_per_us : 0U;

		rt_printf("[IRQPROF][%lu][USB_CH4_FLOW] "
			  "service nak/halt/other=%lu/%lu/%lu "
			  "us_avg/max nak=%lu/%lu halt=%lu/%lu "
			  "halt_call nak/other=%lu/%lu\r\n",
			  (unsigned long)sequence,
			  (unsigned long)nak, (unsigned long)halt,
			  (unsigned long)irqprof_usb_ch4_flow_snapshot[
				IRQPROF_USB_CH4_OTHER_SERVICE],
			  (unsigned long)nak_avg_us,
			  (unsigned long)(cycles_per_us != 0U ?
				irqprof_usb_ch4_flow_snapshot[
					IRQPROF_USB_CH4_NAK_SERVICE_MAX_CYCLES] /
				cycles_per_us : 0U),
			  (unsigned long)halt_avg_us,
			  (unsigned long)(cycles_per_us != 0U ?
				irqprof_usb_ch4_flow_snapshot[
					IRQPROF_USB_CH4_HALT_SERVICE_MAX_CYCLES] /
				cycles_per_us : 0U),
			  (unsigned long)irqprof_usb_ch4_flow_snapshot[
				IRQPROF_USB_CH4_NAK_HALT_CALL],
			  (unsigned long)irqprof_usb_ch4_flow_snapshot[
				IRQPROF_USB_CH4_OTHER_HALT_CALL]);
	}
#endif
#endif
#endif

#if CONFIG_IRQ_PROFILE_USB_CAUSE && CONFIG_IRQ_PROFILE_USB_CH4_SEQUENCE
	{
		uint32_t submit = irqprof_usb_ch4_sequence_snapshot[
			IRQPROF_USB_CH4_SEQ_SUBMIT_FIRST];
		uint32_t halt = irqprof_usb_ch4_sequence_snapshot[
			IRQPROF_USB_CH4_SEQ_HALT_FIRST];
		uint32_t submit_avg_us = submit != 0U ?
			irqprof_usb_ch4_sequence_snapshot[
				IRQPROF_USB_CH4_SEQ_SUBMIT_US] /
				submit : 0U;
		uint32_t halt_avg_us = halt != 0U ?
			irqprof_usb_ch4_sequence_snapshot[
				IRQPROF_USB_CH4_SEQ_HALT_US] /
				halt : 0U;

		rt_printf("[IRQPROF][%lu][USB_CH4_SEQUENCE] "
			  "nak=%lu first submit/halt/same=%lu/%lu/%lu "
			  "overlap=%lu unresolved=%lu "
			  "gap_us submit avg/max=%lu/%lu halt avg/max=%lu/%lu\r\n",
			  (unsigned long)sequence,
			  (unsigned long)irqprof_usb_ch4_sequence_snapshot[
				IRQPROF_USB_CH4_SEQ_NAK],
			  (unsigned long)submit, (unsigned long)halt,
			  (unsigned long)irqprof_usb_ch4_sequence_snapshot[
				IRQPROF_USB_CH4_SEQ_SAME_IRQ],
			  (unsigned long)irqprof_usb_ch4_sequence_snapshot[
				IRQPROF_USB_CH4_SEQ_NAK_OVERLAP],
			  (unsigned long)irqprof_usb_ch4_sequence_pending,
			  (unsigned long)submit_avg_us,
			  (unsigned long)irqprof_usb_ch4_sequence_snapshot[
				IRQPROF_USB_CH4_SEQ_SUBMIT_MAX_US],
			  (unsigned long)halt_avg_us,
			  (unsigned long)irqprof_usb_ch4_sequence_snapshot[
				IRQPROF_USB_CH4_SEQ_HALT_MAX_US]);
	}
#endif

#if CONFIG_IRQ_PROFILE_USB_CH4_NCM
	rt_printf("[IRQPROF][%lu][USB_CH4_NCM] "
		  "get calls/ok/fail=%lu/%lu/%lu "
		  "device in/out_max=%lu/%luB "
		  "set calls/ok/fail=%lu/%lu/%lu requested=%luB "
		  "receive_size=%luB calls=%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)irqprof_usb_ch4_ncm.get_calls,
		  (unsigned long)irqprof_usb_ch4_ncm.get_ok,
		  (unsigned long)irqprof_usb_ch4_ncm.get_fail,
		  (unsigned long)irqprof_usb_ch4_ncm.device_in_max,
		  (unsigned long)irqprof_usb_ch4_ncm.device_out_max,
		  (unsigned long)irqprof_usb_ch4_ncm.set_calls,
		  (unsigned long)irqprof_usb_ch4_ncm.set_ok,
		  (unsigned long)irqprof_usb_ch4_ncm.set_fail,
		  (unsigned long)irqprof_usb_ch4_ncm.set_requested,
		  (unsigned long)irqprof_usb_ch4_ncm.receive_size,
		  (unsigned long)irqprof_usb_ch4_ncm.receive_size_calls);
#endif

#if CONFIG_USB_IRQ_SAFE_DEDUP && CONFIG_IRQ_PROFILE_USB_HANDOFF
	rt_printf("[IRQPROF][%lu][USB_DEDUP] "
		  "calls/nvic_pending/controller_pending/repend=%lu/%lu/%lu/%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)irqprof_usb_dedup_snapshot[
			IRQPROF_USB_DEDUP_CALL],
		  (unsigned long)irqprof_usb_dedup_snapshot[
			IRQPROF_USB_DEDUP_NVIC_PENDING],
		  (unsigned long)irqprof_usb_dedup_snapshot[
			IRQPROF_USB_DEDUP_CONTROLLER_PENDING],
		  (unsigned long)irqprof_usb_dedup_snapshot[
			IRQPROF_USB_DEDUP_REPEND]);
#endif

	for (i = 0U; i < top_count; ++i) {
		uint32_t irq = top[i];
		uint32_t raw = irqprof_snapshot[irq];
		uint32_t adjusted = irqprof_adjusted_count(irq);
		uint32_t pct_100 = adjusted_total != 0U ?
			(uint32_t)(((uint64_t)adjusted * 10000U) /
				   adjusted_total) : 0U;

		printed += adjusted;
		rt_printf("[IRQPROF][%lu][IRQ] #%02lu irq=%lu vector=%lu "
			  "name=%-11s raw/adjusted=%lu/%lu rate=%lu/s "
			  "pct_adjusted=%lu.%02lu%%\r\n",
			  (unsigned long)sequence, (unsigned long)(i + 1U),
			  (unsigned long)irq, (unsigned long)(irq + 16U),
			  irqprof_names[irq], (unsigned long)raw,
			  (unsigned long)adjusted,
			  (unsigned long)(((uint64_t)adjusted * 1000U) /
					  window_ms),
			  (unsigned long)(pct_100 / 100U),
			  (unsigned long)(pct_100 % 100U));
	}
	if (adjusted_total > printed) {
		rt_printf("[IRQPROF][%lu][IRQ] others adjusted=%lu\r\n",
			  (unsigned long)sequence,
			  (unsigned long)(adjusted_total - printed));
	}
}

#else

void carbox_irq_profiler_init(void)
{
}

void carbox_irq_profiler_snapshot(uint32_t profiler_irq,
				  uint32_t profiler_callbacks)
{
	(void)profiler_irq;
	(void)profiler_callbacks;
}

void carbox_irq_profiler_report(uint32_t sequence, uint32_t window_ms)
{
	(void)sequence;
	(void)window_ms;
}

#endif /* CONFIG_IRQ_PROFILE */
