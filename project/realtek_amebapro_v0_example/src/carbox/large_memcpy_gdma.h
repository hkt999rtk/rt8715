#ifndef CARBOX_LARGE_MEMCPY_GDMA_H
#define CARBOX_LARGE_MEMCPY_GDMA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reserve two linked-list-capable GDMA contexts during application startup.
 * The channels remain owned for the lifetime of the firmware. */
void carbox_large_memcpy_gdma_init(void);

/* Print the latched state and fallback counters from the existing ten-second
 * profiling window.  A disabled channel keeps its original failure reason so
 * an early UART message is not required for diagnosis. */
void carbox_large_memcpy_gdma_report(uint32_t sequence);

/* Return non-zero only when the entire memcpy has completed through GDMA.
 * A zero return means the caller must perform the complete CPU copy. */
int carbox_large_memcpy_gdma_try(void *dst, const void *src, size_t len);

/* One logical scatter/gather copy.  Source blocks may be unrelated pbuf
 * allocations; destination blocks normally form one contiguous application
 * buffer.  The implementation keeps unaligned cache-line edges on the CPU and
 * submits only isolated, word-aligned bodies to linked GDMA. */
typedef struct carbox_gdma_copy_block_s {
	void *dst;
	const void *src;
	uint32_t len;
} carbox_gdma_copy_block_t;

typedef struct carbox_gdma_copyv_result_s {
	uint32_t dma_bytes;
	uint32_t cpu_edge_bytes;
	uint16_t dma_blocks;
	uint16_t dma_batches;
} carbox_gdma_copyv_result_t;

/* Return non-zero when every block has been copied, using linked GDMA for the
 * eligible bodies and the CPU for cache-line edges.  Zero means no destination
 * bytes were intentionally committed and the caller must copy every block by
 * CPU.  A runtime DMA error may have partially modified the destination, but
 * the channel is quiesced before zero is returned so a complete CPU recopy is
 * safe. */
int carbox_linked_gdma_copyv_try(const carbox_gdma_copy_block_t *blocks,
				 size_t block_count,
				 carbox_gdma_copyv_result_t *result);

/* Byte-width variant for layouts whose source and destination addresses have
 * different mod-4 values (notably an NCM datagram after its protocol-header
 * pbuf).  Destination DMA bodies remain cache-line isolated exactly as in the
 * validated socket-recv path. */
int carbox_linked_gdma_copyv_bytes_try(const carbox_gdma_copy_block_t *blocks,
				       size_t block_count,
				       carbox_gdma_copyv_result_t *result);

#ifdef __cplusplus
}
#endif

#endif /* CARBOX_LARGE_MEMCPY_GDMA_H */
