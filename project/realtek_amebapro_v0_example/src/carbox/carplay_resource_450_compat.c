#include <stdint.h>
#include <stdio.h>

extern int AirPlayResponse_ParseResource(uint64_t transfer_type,
		uint64_t priority, uint64_t take_constraint,
		uint64_t borrow_constraint);
extern void *GetAirPlayInfo(void);

/* Called only from the two resource-decision sites in GetInfoInfoModes.
 * The vendor code treats every version above 241 as the old 500-priority
 * case.  Limit the experiment to the observed CHERY 450.x response. */
int carbox_parse_resource_450_compat(uint64_t transfer_type,
		uint64_t priority, uint64_t take_constraint,
		uint64_t borrow_constraint)
{
	const uint8_t *info = (const uint8_t *)GetAirPlayInfo();
	uint32_t major = 0;
	uint32_t minor = 0;
	int result = AirPlayResponse_ParseResource(transfer_type, priority,
						     take_constraint,
						     borrow_constraint);

	if (info != NULL) {
		major = *(const uint32_t *)(info + 900);
		minor = *(const uint32_t *)(info + 904);
	}
	if (result == 0 && major == 450 && transfer_type == 1 &&
	    priority == 500 && take_constraint == 500 &&
	    borrow_constraint == 100) {
		printf("[MODE_450] version=%lu.%lu take 500/500/100 result=0->1\n",
		       (unsigned long)major, (unsigned long)minor);
		return 1;
	}
	return result;
}
