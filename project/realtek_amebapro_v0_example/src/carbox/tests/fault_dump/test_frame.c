#include <assert.h>
#include <stdio.h>
#include "fault_dump_frame.h"

int main(void)
{
	uint32_t core, sp = 0x70001000U;
	unsigned i;
	const unsigned unsafe_bits[] = {3, 4, 5, 11, 12, 13, 20};
	/* Basic PSP, basic MSP, extended FP and nonsecure caller. PC is always
	 * word 6 of the core frame; do not add 18 words for FP stacking. */
	assert(carbox_fault_core_frame(sp, 0xfffffffdU, 0, sp, &core) && core == sp);
	assert(carbox_fault_core_frame(sp, 0xfffffff1U, 0, 0, &core) && core == sp);
	assert(carbox_fault_core_frame(sp, 0xffffffedU, 0, 0, &core) && core == sp);
	assert(carbox_fault_core_frame(sp, 0xffffffbdU, 0, 0, &core) && core == sp);
	assert(carbox_fault_core_frame(sp, 0xfffffffdU, 1U << 16, 0, &core));
	assert(carbox_fault_core_frame(sp, 0xfffffffdU, 1U << 24, 0, &core));
	for (i = 0; i < sizeof(unsafe_bits) / sizeof(unsafe_bits[0]); ++i)
		assert(!carbox_fault_core_frame(sp, 0xfffffffdU, 1U << unsafe_bits[i], 0, &core));
	assert(!carbox_fault_core_frame(sp, 0xffffffddU, 0, 0, &core)); /* DCRS */
	assert(!carbox_fault_core_frame(sp, 0xfffffffcU, 0, 0, &core)); /* wrong ES */
	assert(!carbox_fault_core_frame(sp, 0xffffffffU, 0, 0, &core)); /* reserved */
	assert(!carbox_fault_core_frame(sp, 0xfffffff5U, 0, 0, &core)); /* handler PSP */
	assert(!carbox_fault_core_frame(sp, 0x10001001U, 0, 0, &core));
	assert(!carbox_fault_core_frame(sp + 1, 0xfffffffdU, 0, 0, &core));
	assert(!carbox_fault_core_frame(sp, 0xfffffffdU, 0, sp + 4, &core));
	assert(!carbox_fault_core_frame(0x40000000U, 0xfffffffdU, 0, 0, &core));
	assert(!carbox_fault_core_frame(0xfffffff0U, 0xffffffedU, 0, 0, &core));
	assert(carbox_fault_core_frame(0x71ffffe0U, 0xfffffffdU, 0, 0, &core));
	assert(!carbox_fault_core_frame(0x71ffffe0U, 0xffffffedU, 0, 0, &core));
	assert(carbox_fault_ram(0x20000400U, 32));
	assert(carbox_fault_ram(0x20100a00U, 32));
	assert(!carbox_fault_ram(0x200003fcU, 32));
	assert(!carbox_fault_ram(0x2017fff0U, 32));
	assert(!carbox_fault_ram(0x60000000U, 32)); /* not mapped by this LPDDR image */
	assert(!carbox_fault_ram(sp, 0));
	puts("fault frame/range tests PASS");
	return 0;
}
