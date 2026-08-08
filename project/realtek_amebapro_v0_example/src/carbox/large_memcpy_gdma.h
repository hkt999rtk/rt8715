#ifndef CARBOX_LARGE_MEMCPY_GDMA_H
#define CARBOX_LARGE_MEMCPY_GDMA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reserve all four linked-list-capable GDMA contexts during application
 * startup: channel 4 and 5 on both GDMA0 and GDMA1.  The channels remain owned
 * for the lifetime of the firmware. */
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
	uint32_t channel_wait_us;
	/* Phase timings cover one complete logical copyv, including every LLI
	 * batch.  cache_clean_us includes source, destination and descriptor
	 * publication; cache_invalidate_us includes descriptor readback and all
	 * destination visibility passes. */
	uint32_t cache_clean_us;
	uint32_t dma_wait_us;
	uint32_t cache_invalidate_us;
	uint32_t cpu_edge_us;
	uint16_t dma_blocks;
	uint16_t dma_batches;
	uint8_t channel_waited;
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

/* Diagnostic-only nonblocking variant.  It bypasses the normal >4 KiB
 * amortization threshold, but preserves address, alignment, burst, cache,
 * channel and runtime-error validation.  This allows one real TCP pbuf to be
 * isolated without changing production copyv policy. */
int carbox_linked_gdma_copyv_force_try(
	const carbox_gdma_copy_block_t *blocks, size_t block_count,
	carbox_gdma_copyv_result_t *result);

/* Task-only variant for callers which must not fall back merely because all
 * linked channels are occupied.  It blocks on the channel counting semaphore,
 * yielding the CPU until a channel is released.  It still returns zero for an
 * unsuitable request, no usable linked channel, or a runtime hardware error. */
int carbox_linked_gdma_copyv_wait(const carbox_gdma_copy_block_t *blocks,
				  size_t block_count,
				  carbox_gdma_copyv_result_t *result);

#ifdef __cplusplus
}
#endif

#endif /* CARBOX_LARGE_MEMCPY_GDMA_H */
