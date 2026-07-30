/*
 * Smart libusb_ref HAL/SoC compatibility for manual Pro1 link experiments.
 *
 * These names come from the Smart USB stack. Some can map to Pro1 HAL
 * primitives; USB-chip functions still require a real Pro1 USB bring-up port.
 */

#include <stdint.h>

#include "FreeRTOS.h"
#include "cmsis.h"
#include "hal_cache.h"
#include "hal_irq.h"
#include "task.h"
#include "usb_otg/usb.h"

/*
 * Smart SDK SoC primitives �� Pro1 SDK may define HAL_READ32/HAL_WRITE32
 * as macros.  Undefine them so we can provide linker-visible symbols
 * that usb_hal.o (precompiled in libusb_ref.a) can resolve.
 */
void DelayUs(uint32_t us)
{
	extern void hal_delay_us(uint32_t time_us);
	hal_delay_us(us);
}

#define CARBOX_LIBUSB_REF_CACHE_ALL_ADDR 0xffffffffU

typedef void (*carbox_libusb_ref_irq_handler_t)(void *data);

void InterruptEn(int32_t irqn, uint32_t priority)
{
	hal_irq_set_priority(irqn, priority);
	hal_irq_enable(irqn);
}

void InterruptDis(int32_t irqn)
{
	hal_irq_disable(irqn);
}

uint32_t InterruptRegister(carbox_libusb_ref_irq_handler_t handler,
			   int32_t irqn, uint32_t data, uint32_t priority)
{
	(void)data;

	hal_irq_set_vector(irqn, (uint32_t)handler);
	hal_irq_set_priority(irqn, priority);

	return 1U;
}

void InterruptUnRegister(int32_t irqn)
{
	hal_int_vector_stubs.hal_irq_unreg(irqn);
}

void DCache_Clean(uint32_t addr, uint32_t size)
{
	if (size == 0U) {
		return;
	}

	if (addr == CARBOX_LIBUSB_REF_CACHE_ALL_ADDR &&
	    size == CARBOX_LIBUSB_REF_CACHE_ALL_ADDR) {
		dcache_clean();
		return;
	}

	dcache_clean_by_addr((uint32_t *)addr, (int32_t)size);
}

void DCache_Invalidate(uint32_t addr, uint32_t size)
{
	if (size == 0U) {
		return;
	}

	if (addr == CARBOX_LIBUSB_REF_CACHE_ALL_ADDR &&
	    size == CARBOX_LIBUSB_REF_CACHE_ALL_ADDR) {
		dcache_invalidate();
		return;
	}

	dcache_invalidate_by_addr((uint32_t *)addr, (int32_t)size);
}