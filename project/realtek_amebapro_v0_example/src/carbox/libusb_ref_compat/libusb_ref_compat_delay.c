#include <stdint.h>

#include "cmsis.h"

extern uint32_t SystemCoreClock;
extern void hal_delay_us(uint32_t time_us);

#define USB_DWT_DELAY_MAX_US 10U
#define USB_DWT_STALL_READS 32U

/* USB PHY uses DelayUs(1). Keep the longer OS/NCM delays on the GTimer.
 * The boot clock setup maintains SystemCoreClock; CPU frequency must remain
 * unchanged during this call. Neither task priority nor IRQ state is changed.
 * DWT is shared with runtime statistics: never reset or reconfigure it here.
 */
void DelayUs(uint32_t us)
{
	uint32_t clock_hz;
	uint32_t start;
	uint32_t previous;
	uint32_t stalled = 0U;
	uint32_t cycles;
	uint64_t scaled_cycles;

	if (us == 0U)
		return;
	clock_hz = SystemCoreClock;
	if (us > USB_DWT_DELAY_MAX_US || clock_hz == 0U ||
	    (CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk) == 0U ||
	    (DWT->CTRL & (DWT_CTRL_NOCYCCNT_Msk | DWT_CTRL_CYCCNTENA_Msk)) !=
		DWT_CTRL_CYCCNTENA_Msk)
		goto fallback;

	/* Widen before multiplying and round up, including non-integer MHz clocks.
	 * Short requests keep the target well below one 32-bit counter period.
	 */
	scaled_cycles = (uint64_t)clock_hz * us + 999999ULL;
	/* 300/400 MHz and <=10 us fit this branch: avoid a software 64-bit
	 * division on every PHY poll, while retaining safe wide arithmetic.
	 */
	if (scaled_cycles <= UINT32_MAX)
		cycles = (uint32_t)scaled_cycles / 1000000U;
	else
		cycles = (uint32_t)(scaled_cycles / 1000000ULL);
	start = DWT->CYCCNT;
	previous = start;
	for (;;) {
		uint32_t now = DWT->CYCCNT;

		/* Unsigned subtraction handles a counter rollover during the wait.
		 * Preemption may extend the delay; this is not a scheduling barrier.
		 */
		if ((uint32_t)(now - start) >= cycles)
			return;
		if (now == previous) {
			if (++stalled >= USB_DWT_STALL_READS)
				goto fallback;
		} else {
			stalled = 0U;
		}
		previous = now;
		__NOP();
	}

fallback:
	/* Restart the full wait conservatively if DWT stalls part-way through.
	 * This retains the existing GTimer's availability requirements.
	 */
	hal_delay_us(us);
}
