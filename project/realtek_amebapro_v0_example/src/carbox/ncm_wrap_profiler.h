#ifndef CARBOX_NCM_WRAP_PROFILER_H
#define CARBOX_NCM_WRAP_PROFILER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void ncm_wrap_profiler_report(uint32_t sequence);

/* Experimental chained-pbuf copy elision.  prepare() reserves the currently
 * published customer NTB payload for one synchronous send.  cancel() is only
 * needed when flattening fails before usbh_cdc_ncm_send_data() is called. */
int ncm_wrap_copy_elide_prepare(uint32_t packet_length, void **payload,
	void **allocation_end, uint32_t *token);
void ncm_wrap_copy_elide_cancel(uint32_t token);

/* Called only by the global libc wrappers while the customer builder is in
 * the scoped prepared transaction.  A nonzero result means the operation was
 * handled and the normal libc operation must be skipped. */
int ncm_wrap_copy_elide_memset(void *dst, int value, size_t length);
int ncm_wrap_copy_elide_memcpy(void *dst, const void *src, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* CARBOX_NCM_WRAP_PROFILER_H */
