#pragma once
#include <stdint.h>

#define CoreDebug_DEMCR_TRCENA_Msk (1UL << 24)
#define DWT_CTRL_CYCCNTENA_Msk 1UL
#define DWT_CTRL_NOCYCCNT_Msk (1UL << 25)

uint32_t mock_cycle_read(void);
struct MockCycleRegister {
	operator uint32_t() const { return mock_cycle_read(); }
};
struct MockDwt {
	uint32_t CTRL;
	MockCycleRegister CYCCNT;
};
struct MockCoreDebug { uint32_t DEMCR; };
extern MockDwt mock_dwt;
extern MockCoreDebug mock_debug;
#define DWT (&mock_dwt)
#define CoreDebug (&mock_debug)
#define __NOP() ((void)0)
