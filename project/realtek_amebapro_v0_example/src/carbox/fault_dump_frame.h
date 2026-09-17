#ifndef CARBOX_FAULT_DUMP_FRAME_H
#define CARBOX_FAULT_DUMP_FRAME_H

#include <stdint.h>

/* This IS image uses LPDDR. Do not probe peripherals, flash or arbitrary RAM. */
static inline int carbox_fault_ram(uint32_t addr, uint32_t bytes)
{
	if (!bytes || bytes > UINT32_MAX - addr) return 0;
	return (addr >= 0x20000400U && addr + bytes <= 0x20010000U) ||
	       (addr >= 0x20100a00U && addr + bytes <= 0x20180000U) ||
	       (addr >= 0x70000000U && addr + bytes <= 0x72000000U);
}

/* Reject stacking faults and Armv8-M additional-state frames rather than
 * misreporting their contents as PC/LR. Both basic and FP extended frames
 * begin with the eight core words. FP state follows them at HIGHER addresses
 * (Armv8-M Exception Model User Guide, exception stack frame layout). */
static inline int carbox_fault_core_frame(uint32_t sp, uint32_t exc_return,
		uint32_t cfsr, uint32_t limit, uint32_t *core)
{
	uint32_t bytes = (exc_return & (1U << 4)) ? 32U : 104U;
	if ((exc_return & 0xffffff80U) != 0xffffff80U ||
	    !(exc_return & 1U) || (exc_return & 2U) || !(exc_return & (1U << 5)) ||
	    (!(exc_return & 8U) && (exc_return & 4U)) ||
	    (cfsr & ((7U << 3) | (7U << 11) | (1U << 20))) ||
	    (sp & 3U) || sp < limit ||
	    !carbox_fault_ram(sp, bytes)) return 0;
	*core = sp;
	return 1;
}

#endif
