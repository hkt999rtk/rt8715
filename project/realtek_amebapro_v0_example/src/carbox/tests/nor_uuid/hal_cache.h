#ifndef NOR_TEST_HAL_CACHE_H
#define NOR_TEST_HAL_CACHE_H
#include <stdint.h>
#define SCB_CCR_DC_Msk (1U << 16)
#define SCB_CCR_IC_Msk (1U << 17)
struct NorMockSCB { uint32_t CCR; };
extern NorMockSCB nor_mock_scb;
#define SCB (&nor_mock_scb)
uint32_t __get_PRIMASK(void);
void __disable_irq(void);
void __set_PRIMASK(uint32_t value);
void dcache_clean(void);
void dcache_disable(void);
void icache_disable(void);
void dcache_enable(void);
void icache_invalidate(void);
void icache_enable(void);
#endif
