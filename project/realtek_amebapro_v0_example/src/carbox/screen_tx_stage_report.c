#include "screen_tx_direct_crypto.h"

#include "diag.h"


void carbox_screen_tx_stage_report(uint32_t sequence)
{
	carbox_screen_tx_stage_snapshot_t stats;

	if (!carbox_screen_tx_stage_snapshot(&stats)) {
		return;
	}
	rt_printf("[SCREENTXSTAGE][%lu] frames/full/partial/unmatched=%lu/%lu/%lu/%lu "
		  "prepare_us avg/max=%llu/%lu crypto_us avg/max=%llu/%lu "
		  "post_crypto_us avg/max=%llu/%lu "
		  "service_us avg/max=%llu/%lu over16/33ms=%lu/%lu "
		  "boundary=wire-allocation-to-full-lwip-write-accepted\r\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.frames,
		  (unsigned long)stats.full_writes,
		  (unsigned long)stats.partial_writes,
		  (unsigned long)stats.unmatched_writes,
		  (unsigned long long)(stats.frames != 0U ?
			stats.prepare_sum_us / stats.frames : 0U),
		  (unsigned long)stats.prepare_max_us,
		  (unsigned long long)(stats.frames != 0U ?
			stats.crypto_sum_us / stats.frames : 0U),
		  (unsigned long)stats.crypto_max_us,
		  (unsigned long long)(stats.frames != 0U ?
			stats.post_crypto_sum_us / stats.frames : 0U),
		  (unsigned long)stats.post_crypto_max_us,
		  (unsigned long long)(stats.frames != 0U ?
			stats.service_sum_us / stats.frames : 0U),
		  (unsigned long)stats.service_max_us,
		  (unsigned long)stats.service_over_16ms,
		  (unsigned long)stats.service_over_33ms);
	rt_printf("[SCREENTXBREAK][%lu] prepare alloc_to_header/header_to_payload/"
		  "payload_to_crypto_us avg:max=%llu:%lu/%llu:%lu/%llu:%lu "
		  "post crypto_to_write/write_call_us avg:max=%llu:%lu/%llu:%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long long)(stats.frames != 0U ?
			stats.alloc_to_header_sum_us / stats.frames : 0U),
		  (unsigned long)stats.alloc_to_header_max_us,
		  (unsigned long long)(stats.frames != 0U ?
			stats.header_to_payload_sum_us / stats.frames : 0U),
		  (unsigned long)stats.header_to_payload_max_us,
		  (unsigned long long)(stats.frames != 0U ?
			stats.payload_to_crypto_sum_us / stats.frames : 0U),
		  (unsigned long)stats.payload_to_crypto_max_us,
		  (unsigned long long)(stats.frames != 0U ?
			stats.crypto_to_write_sum_us / stats.frames : 0U),
		  (unsigned long)stats.crypto_to_write_max_us,
		  (unsigned long long)(stats.frames != 0U ?
			stats.write_call_sum_us / stats.frames : 0U),
		  (unsigned long)stats.write_call_max_us);
}
