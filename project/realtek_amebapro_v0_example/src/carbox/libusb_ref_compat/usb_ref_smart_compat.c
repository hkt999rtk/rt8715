#include <stdint.h>

#include "hal_cache.h"
#include "hal_irq.h"

extern void rtos_time_delay_ms(uint32_t ms);
extern uint64_t rtos_time_get_current_system_time_us(void);

uint64_t DTimestamp_Get(void)
{
	return rtos_time_get_current_system_time_us();
}

#ifdef hal_delay_ms
#undef hal_delay_ms
#endif

void hal_delay_ms(uint32_t ms)
{
	rtos_time_delay_ms(ms);
}

void hal_irq_enable_rtl8195bhp(int32_t irqn)
{
	hal_irq_enable(irqn);
}

void hal_irq_disable_rtl8195bhp(int32_t irqn)
{
	hal_irq_disable(irqn);
}

void hal_irq_set_vector_rtl8195bhp(int32_t irqn, uint32_t vector)
{
	hal_irq_set_vector(irqn, vector);
}

void hal_irq_set_priority_rtl8195bhp(int32_t irqn, uint32_t priority)
{
	hal_irq_set_priority(irqn, priority);
}

void hal_irq_unreg_rtl8195bhp(int32_t irqn)
{
	hal_irq_disable(irqn);
}
