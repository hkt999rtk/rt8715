#include "lib_rtk264.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "dma_api.h"
#include "hal_cache.h"
#include "hal_gdma.h"
#include "h264_api.h"
#include "osdep_service.h"

#define RTK264_HW_ALIGNMENT       32U
#define RTK264_DIM_ALIGNMENT      16U
#define RTK264_MIN_WIDTH          132U
#define RTK264_MAX_WIDTH          1920U
#define RTK264_MIN_HEIGHT         96U
#define RTK264_MAX_HEIGHT         4080U
#define RTK264_DEFAULT_BITRATE    (10U * 1024U * 1024U)
#define RTK264_DEFAULT_GOP        1024U
#define RTK264_GDMA_TIMEOUT_MS     50U
/* 16 blocks x 4095 byte transfers, rounded down to a cache line. */
#define RTK264_GDMA_CHUNK_SIZE  65504U
#define RTK264_GDMA_SELFTEST_SIZE 4096U
#define RTK264_GDMA_OUTPUT_THRESHOLD 4096U
#define RTK264_GDMA_REPORT_FRAMES  300U

#ifndef RTK264_USE_GDMA
#define RTK264_USE_GDMA 1
#endif

#ifndef RTK264_GDMA_SELFTEST
#define RTK264_GDMA_SELFTEST 1
#endif

#ifndef RTK264_GDMA_STATS
#define RTK264_GDMA_STATS 1
#endif

/* These SDK entry points are exported by lib_h264.a but have no public header. */
extern void hal_video_sys_init_rtl8195bhp(void);
extern void hal_video_sys_deinit_rtl8195bhp(void);
extern void hal_enc_hw_init(void);
extern void hal_enc_hw_deinit(void);
extern void init_h1v6_parm(void);
extern void deinit_h1v6_parm(void);

typedef struct {
	gdma_t dma;
	_sema done;
	int initialized;
	int available;
	volatile int in_flight;
	volatile int irq_error;
	volatile uint32_t irq_raw_error;
	uint32_t frames;
	uint32_t dma_frames;
	uint32_t cpu_frames;
	uint32_t fallback_frames;
	uint32_t dma_ops;
	uint64_t dma_bytes;
} rtk264_gdma_state_t;

typedef struct {
	struct h264_context *encoder;
	void *staging_allocation;
	uint8_t *staging;
	size_t staging_size;
	void *output_allocation;
	uint8_t *output;
	size_t output_capacity;
	size_t input_size;
	uint32_t width;
	uint32_t height;
	uint32_t aligned_width;
	uint32_t aligned_height;
	int hardware_initialized;
	int initialized;
	int encoding;
	rtk264_gdma_state_t y_gdma;
} rtk264_state_t;

static rtk264_state_t g_rtk264;

static int rtk264_dma_accessible(const void *address, size_t size)
{
	uintptr_t start = (uintptr_t)address;
	uintptr_t end = start + size;

	if (end < start) {
		return 0;
	}
	return ((start >= 0x20100000U && end <= 0x2017A000U) ||
		(start >= 0x60000000U && end <= 0x60800000U) ||
		(start >= 0x70000000U && end <= 0x72000000U));
}

static void rtk264_gdma_done(uint32_t id)
{
	rtk264_gdma_state_t *state = &g_rtk264.y_gdma;
	phal_gdma_adaptor_t adaptor = &state->dma.hal_gdma_adaptor;
	uint32_t channel_mask;

	(void)id;
	if (!state->in_flight || state->done == NULL ||
	    adaptor->gdma_dev == NULL) {
		return;
	}
	channel_mask = 1U << adaptor->ch_num;
	state->irq_raw_error = adaptor->gdma_dev->raw_err & channel_mask;
	if (state->irq_raw_error != 0U) {
		state->irq_error = 1;
	}
	state->in_flight = 0;
	rtw_up_sema_from_isr(&state->done);
}

static void rtk264_gdma_drain(rtk264_gdma_state_t *state)
{
	while (rtw_down_timeout_sema(&state->done, 0) == _TRUE) {
	}
}

static void rtk264_gdma_disable(rtk264_gdma_state_t *state,
				const char *reason)
{
	phal_gdma_adaptor_t adaptor = &state->dma.hal_gdma_adaptor;

	/*
	 * A failed single-block to multi-block channel migration leaves
	 * have_chnl clear.  Never abort/free using the stale channel number in
	 * that case because another GDMA client may already own that channel.
	 */
	if (state->available && adaptor->have_chnl) {
		hal_gdma_isr_dis(adaptor);
		hal_gdma_abort(adaptor);
		hal_gdma_clean_pending_isr(adaptor);
		dma_memcpy_deinit(&state->dma);
	}
	state->available = 0;
	state->in_flight = 0;
	printf("[RTK264][GDMA][ERROR] disabled reason=%s raw_err=0x%08x; "
	       "CPU fallback active\n", reason,
	       (unsigned int)state->irq_raw_error);
}

static int rtk264_gdma_start(rtk264_gdma_state_t *state, void *dst,
			     const void *src, uint32_t length)
{
	phal_gdma_adaptor_t adaptor = &state->dma.hal_gdma_adaptor;
	hal_status_t status;

	if (!state->available || length == 0U ||
	    !rtk264_dma_accessible(dst, length) ||
	    !rtk264_dma_accessible(src, length)) {
		return -1;
	}

	/*
	 * Destination is private, 32-byte-aligned staging memory.  Discard any
	 * old dirty lines before GDMA writes it, otherwise a later eviction could
	 * overwrite the freshly copied luma data.
	 */
	dcache_clean_invalidate_by_addr((uint32_t *)dst, (int32_t)length);
	rtk264_gdma_drain(state);
	hal_gdma_clean_pending_isr(adaptor);
	state->irq_error = 0;
	state->irq_raw_error = 0U;
	state->in_flight = 1;
	/* dma_memcpy() discards this status, which hides channel migration errors. */
	status = hal_gdma_memcpy(adaptor, dst, (void *)src, length);
	if (status != HAL_OK) {
		state->in_flight = 0;
		rtk264_gdma_disable(state, "start-error");
		return -1;
	}
	return 0;
}

static int rtk264_gdma_wait(rtk264_gdma_state_t *state, void *dst,
			    uint32_t length)
{
	if (rtw_down_timeout_sema(&state->done,
				  RTK264_GDMA_TIMEOUT_MS) != _TRUE) {
		rtk264_gdma_disable(state, "timeout");
		return -1;
	}
	if (state->irq_error) {
		rtk264_gdma_disable(state, "controller-error");
		return -1;
	}

	/* CPU may subsequently touch row padding in these DMA-written lines. */
	dcache_invalidate_by_addr((uint32_t *)dst, (int32_t)length);
	state->dma_ops++;
	state->dma_bytes += length;
	return 0;
}

static int rtk264_gdma_selftest(rtk264_gdma_state_t *state)
{
#if RTK264_GDMA_SELFTEST
	uint8_t *src = g_rtk264.output;
	uint8_t *dst = g_rtk264.staging;
	uint32_t length = RTK264_GDMA_SELFTEST_SIZE;
	uint32_t index;

	if (g_rtk264.output_capacity < length || g_rtk264.staging_size < length) {
		length = (uint32_t)(g_rtk264.output_capacity < g_rtk264.staging_size ?
			g_rtk264.output_capacity : g_rtk264.staging_size);
		length &= ~(RTK264_HW_ALIGNMENT - 1U);
	}
	if (length == 0U) {
		return -1;
	}
	for (index = 0; index < length; ++index) {
		src[index] = (uint8_t)(index * 29U + 7U);
	}
	memset(dst, 0, length);
	if (rtk264_gdma_start(state, dst, src, length) < 0 ||
	    rtk264_gdma_wait(state, dst, length) < 0 ||
	    memcmp(dst, src, length) != 0) {
		if (state->available) {
			rtk264_gdma_disable(state, "selftest-mismatch");
		}
		/* Do not leave diagnostic data resident in the encoder output cache. */
		dcache_clean_invalidate_by_addr((uint32_t *)src,
						(int32_t)length);
		return -1;
	}
	/* The first hardware encode must see an output buffer with no dirty lines. */
	dcache_clean_invalidate_by_addr((uint32_t *)src, (int32_t)length);
	printf("[RTK264][GDMA][SELFTEST] PASS len=%u\n",
	       (unsigned int)length);
#else
	(void)state;
#endif
	return 0;
}

static void rtk264_gdma_init(void)
{
#if RTK264_USE_GDMA
	rtk264_gdma_state_t *state = &g_rtk264.y_gdma;
	phal_gdma_adaptor_t adaptor;

	state->initialized = 1;
	rtw_init_sema(&state->done, 0);
	if (state->done == NULL) {
		printf("[RTK264][GDMA] semaphore allocation failed; CPU copy only\n");
		return;
	}
	dma_memcpy_init(&state->dma, rtk264_gdma_done, 0);
	adaptor = &state->dma.hal_gdma_adaptor;
	if (!adaptor->have_chnl) {
		printf("[RTK264][GDMA] channel allocation failed; CPU copy only\n");
		return;
	}

	/* Error-only IRQs are diagnosed by the bounded timeout and raw_err. */
	hal_gdma_isr_dis(adaptor);
	hal_gdma_clean_pending_isr(adaptor);
	adaptor->gdma_isr_type = TransferType;
	hal_gdma_isr_en(adaptor);
	state->available = 1;
	printf("[RTK264][GDMA] Y-plane channel gdma=%u channel=%u chunk=%u\n",
	       (unsigned int)adaptor->gdma_index,
	       (unsigned int)adaptor->ch_num,
	       (unsigned int)RTK264_GDMA_CHUNK_SIZE);
	(void)rtk264_gdma_selftest(state);
#endif
}

static void rtk264_gdma_deinit(void)
{
	rtk264_gdma_state_t *state = &g_rtk264.y_gdma;
	phal_gdma_adaptor_t adaptor = &state->dma.hal_gdma_adaptor;

	if (!state->initialized) {
		return;
	}
	if (adaptor->have_chnl) {
		dma_memcpy_deinit(&state->dma);
	}
	if (state->done != NULL) {
		rtw_free_sema(&state->done);
	}
}

static uint32_t rtk264_align_up_u32(uint32_t value, uint32_t alignment)
{
	return (value + alignment - 1U) & ~(alignment - 1U);
}

static int rtk264_mul_size(size_t a, size_t b, size_t *result)
{
	if (a != 0U && b > SIZE_MAX / a) {
		return -1;
	}
	*result = a * b;
	return 0;
}

static void *rtk264_alloc_aligned(size_t size, void **allocation)
{
	uintptr_t address;
	void *raw;

	if (size > SIZE_MAX - (RTK264_HW_ALIGNMENT - 1U)) {
		return NULL;
	}

	raw = pvPortMalloc(size + RTK264_HW_ALIGNMENT - 1U);
	if (raw == NULL) {
		return NULL;
	}

	address = ((uintptr_t)raw + RTK264_HW_ALIGNMENT - 1U) &
		  ~((uintptr_t)RTK264_HW_ALIGNMENT - 1U);
	*allocation = raw;
	return (void *)address;
}

static void rtk264_release_encoder(void)
{
	rtk264_gdma_deinit();
	if (g_rtk264.encoder != NULL) {
		h264_release(g_rtk264.encoder);
		vPortFree(g_rtk264.encoder);
	}
	if (g_rtk264.staging_allocation != NULL) {
		vPortFree(g_rtk264.staging_allocation);
	}
	if (g_rtk264.output_allocation != NULL) {
		vPortFree(g_rtk264.output_allocation);
	}
	if (g_rtk264.hardware_initialized) {
		deinit_h1v6_parm();
		hal_enc_hw_deinit();
		hal_video_sys_deinit_rtl8195bhp();
	}
	memset(&g_rtk264, 0, sizeof(g_rtk264));
}

static void rtk264_copy_y_cpu(const uint8_t *src_y, size_t y_stride)
{
	const uint32_t width = g_rtk264.width;
	const uint32_t height = g_rtk264.height;
	const uint32_t aligned_width = g_rtk264.aligned_width;
	const uint32_t aligned_height = g_rtk264.aligned_height;
	uint8_t *dst_y = g_rtk264.staging;
	uint32_t row;

	for (row = 0; row < height; ++row) {
		const uint8_t *src = src_y + (size_t)row * y_stride;
		uint8_t *dst = dst_y + (size_t)row * aligned_width;

		memcpy(dst, src, width);
		if (aligned_width > width) {
			memset(dst + width, src[width - 1U], aligned_width - width);
		}
	}
	for (; row < aligned_height; ++row) {
		memcpy(dst_y + (size_t)row * aligned_width,
		       dst_y + (size_t)(height - 1U) * aligned_width,
		       aligned_width);
	}
}

static void rtk264_fix_y_padding(void)
{
	const uint32_t width = g_rtk264.width;
	const uint32_t height = g_rtk264.height;
	const uint32_t aligned_width = g_rtk264.aligned_width;
	const uint32_t aligned_height = g_rtk264.aligned_height;
	uint8_t *dst_y = g_rtk264.staging;
	uint32_t row;

	/*
	 * The legacy input contains padded rows, allowing one contiguous GDMA
	 * copy.  Do not trust its right/bottom padding contents, however: restore
	 * the exact edge-extension behavior of the original CPU implementation.
	 */
	if (aligned_width > width) {
		for (row = 0; row < height; ++row) {
			uint8_t *dst = dst_y + (size_t)row * aligned_width;

			memset(dst + width, dst[width - 1U], aligned_width - width);
		}
	}
	for (row = height; row < aligned_height; ++row) {
		memcpy(dst_y + (size_t)row * aligned_width,
		       dst_y + (size_t)(height - 1U) * aligned_width,
		       aligned_width);
	}
}

static void rtk264_copy_uv_cpu(const uint8_t *src_u,
			       const uint8_t *src_v,
			       size_t chroma_stride)
{
	const uint32_t width = g_rtk264.width;
	const uint32_t height = g_rtk264.height;
	const uint32_t aligned_width = g_rtk264.aligned_width;
	const uint32_t aligned_height = g_rtk264.aligned_height;
	const uint32_t chroma_width = width / 2U;
	const uint32_t chroma_height = height / 2U;
	uint8_t *dst_uv = g_rtk264.staging +
		(size_t)aligned_width * aligned_height;
	uint32_t row;

	for (row = 0; row < chroma_height; ++row) {
		const uint8_t *src_u_row = src_u + (size_t)row * chroma_stride;
		const uint8_t *src_v_row = src_v + (size_t)row * chroma_stride;
		uint8_t *dst = dst_uv + (size_t)row * aligned_width;
		uint32_t column = 0U;

		/*
		 * Cortex-M33 has no NEON.  Interleave four U/V samples per iteration
		 * with 32-bit loads and SWAR bit placement instead of eight scalar
		 * byte stores.  PRO1 is little-endian; the masks below therefore emit
		 * U0,V0,U1,V1 followed by U2,V2,U3,V3.  memcpy keeps unaligned input
		 * legal and optimizes to normal word loads on this target.
		 */
		for (; column + 4U <= chroma_width; column += 4U) {
			uint32_t u;
			uint32_t v;
			uint32_t low;
			uint32_t high;

			memcpy(&u, src_u_row + column, sizeof(u));
			memcpy(&v, src_v_row + column, sizeof(v));
			low = (u & 0x000000FFU) |
			      ((v & 0x000000FFU) << 8) |
			      ((u & 0x0000FF00U) << 8) |
			      ((v & 0x0000FF00U) << 16);
			high = ((u >> 16) & 0x000000FFU) |
			       (((v >> 16) & 0x000000FFU) << 8) |
			       (((u >> 24) & 0x000000FFU) << 16) |
			       (((v >> 24) & 0x000000FFU) << 24);
			memcpy(dst + column * 2U, &low, sizeof(low));
			memcpy(dst + column * 2U + sizeof(low),
			       &high, sizeof(high));
		}

		for (; column < chroma_width; ++column) {
			dst[column * 2U] = src_u_row[column];
			dst[column * 2U + 1U] = src_v_row[column];
		}
		for (column = width; column < aligned_width; column += 2U) {
			dst[column] = src_u_row[chroma_width - 1U];
			dst[column + 1U] = src_v_row[chroma_width - 1U];
		}
	}
	for (; row < aligned_height / 2U; ++row) {
		memcpy(dst_uv + (size_t)row * aligned_width,
		       dst_uv + (size_t)(chroma_height - 1U) * aligned_width,
		       aligned_width);
	}
}

static void rtk264_gdma_report(rtk264_gdma_state_t *state)
{
#if RTK264_GDMA_STATS
	if (state->frames < RTK264_GDMA_REPORT_FRAMES) {
		return;
	}
	printf("[RTK264][GDMA][STATS] frames=%u dma=%u cpu=%u fallback=%u "
	       "ops=%u bytes=%uKiB\n",
	       (unsigned int)state->frames,
	       (unsigned int)state->dma_frames,
	       (unsigned int)state->cpu_frames,
	       (unsigned int)state->fallback_frames,
	       (unsigned int)state->dma_ops,
	       (unsigned int)(state->dma_bytes / 1024U));
	state->frames = 0U;
	state->dma_frames = 0U;
	state->cpu_frames = 0U;
	state->fallback_frames = 0U;
	state->dma_ops = 0U;
	state->dma_bytes = 0U;
#else
	(void)state;
#endif
}

static void rtk264_copy_i420_to_nv12(const uint8_t *src_y,
				     const uint8_t *src_u,
				     const uint8_t *src_v,
				     size_t y_stride,
				     size_t chroma_stride)
{
	rtk264_gdma_state_t *state = &g_rtk264.y_gdma;
	const size_t dma_total = (size_t)g_rtk264.aligned_width *
		g_rtk264.height;
	size_t offset = 0U;
	uint32_t active_length = 0U;
	int dma_started = 0;
	int dma_failed = 0;

	state->frames++;
	/*
	 * With the legacy ABI, source and destination luma have the same padded
	 * stride.  Start the first chunk before UV conversion so GDMA and the CPU
	 * work in parallel.  Compact, non-padded input keeps the row-copy path.
	 */
	if (state->available && y_stride == g_rtk264.aligned_width &&
	    rtk264_dma_accessible(src_y, dma_total) &&
	    rtk264_dma_accessible(g_rtk264.staging, dma_total)) {
		active_length = (uint32_t)(dma_total > RTK264_GDMA_CHUNK_SIZE ?
			RTK264_GDMA_CHUNK_SIZE : dma_total);
		if (rtk264_gdma_start(state, g_rtk264.staging, src_y,
				       active_length) == 0) {
			dma_started = 1;
		}
	}

	rtk264_copy_uv_cpu(src_u, src_v, chroma_stride);

	while (dma_started) {
		if (rtk264_gdma_wait(state, g_rtk264.staging + offset,
				       active_length) < 0) {
			dma_failed = 1;
			break;
		}
		offset += active_length;
		if (offset >= dma_total) {
			break;
		}
		active_length = (uint32_t)(dma_total - offset >
			RTK264_GDMA_CHUNK_SIZE ? RTK264_GDMA_CHUNK_SIZE :
			(dma_total - offset));
		if (rtk264_gdma_start(state, g_rtk264.staging + offset,
				       src_y + offset, active_length) < 0) {
			dma_failed = 1;
			break;
		}
	}

	if (dma_started && !dma_failed && offset == dma_total) {
		rtk264_fix_y_padding();
		state->dma_frames++;
	} else {
		/* A failed DMA may have partially written Y; overwrite all of it. */
		rtk264_copy_y_cpu(src_y, y_stride);
		if (dma_started || dma_failed) {
			state->fallback_frames++;
		} else {
			state->cpu_frames++;
		}
	}
	rtk264_gdma_report(state);
}

static void rtk264_copy_bitstream(uint8_t *dst, const uint8_t *src,
				  uint32_t length)
{
	rtk264_gdma_state_t *state = &g_rtk264.y_gdma;
	uintptr_t dst_address = (uintptr_t)dst;
	uint32_t prefix;
	uint32_t body;
	uint32_t offset;

	if (length < RTK264_GDMA_OUTPUT_THRESHOLD || !state->available) {
		memcpy(dst, src, length);
		return;
	}

	/*
	 * Only invalidate complete destination cache lines.  The caller owns the
	 * bytes surrounding an unaligned bitstream, so copying its two edges on
	 * the CPU prevents GDMA cache maintenance from discarding unrelated data.
	 */
	prefix = (uint32_t)((RTK264_HW_ALIGNMENT -
		(dst_address & (RTK264_HW_ALIGNMENT - 1U))) &
		(RTK264_HW_ALIGNMENT - 1U));
	if (prefix > length) {
		prefix = length;
	}
	body = (length - prefix) & ~(RTK264_HW_ALIGNMENT - 1U);
	if (body < RTK264_GDMA_OUTPUT_THRESHOLD ||
	    !rtk264_dma_accessible(dst + prefix, body) ||
	    !rtk264_dma_accessible(src + prefix, body)) {
		memcpy(dst, src, length);
		return;
	}

	if (prefix != 0U) {
		memcpy(dst, src, prefix);
	}
	offset = 0U;
	while (offset < body) {
		uint32_t chunk = body - offset > RTK264_GDMA_CHUNK_SIZE ?
			RTK264_GDMA_CHUNK_SIZE : body - offset;

		if (rtk264_gdma_start(state, dst + prefix + offset,
				       src + prefix + offset, chunk) < 0 ||
		    rtk264_gdma_wait(state, dst + prefix + offset, chunk) < 0) {
			/* The source remains intact; overwrite any partial DMA result. */
			memcpy(dst + prefix + offset, src + prefix + offset,
			       body - offset);
			break;
		}
		offset += chunk;
	}
	if (prefix + body < length) {
		memcpy(dst + prefix + body, src + prefix + body,
		       length - prefix - body);
	}
}

int lib_rtk264_init(int width, int height, int fps)
{
	struct h264_parameter parameter;
	size_t pixels;
	size_t staging_size;
	size_t input_size;
	uint32_t aligned_width;
	uint32_t aligned_height;
	int ret;

	if (width <= 0 || height <= 0 || fps <= 0 ||
	    ((uint32_t)width & 3U) != 0U || (height & 1) != 0 ||
	    (uint32_t)width < RTK264_MIN_WIDTH ||
	    (uint32_t)width > RTK264_MAX_WIDTH ||
	    (uint32_t)height < RTK264_MIN_HEIGHT ||
	    (uint32_t)height > RTK264_MAX_HEIGHT) {
		return RTK264_ERR_ARGUMENT;
	}
	if (g_rtk264.encoding) {
		return RTK264_ERR_BUSY;
	}

	aligned_width = rtk264_align_up_u32((uint32_t)width,
					     RTK264_DIM_ALIGNMENT);
	aligned_height = rtk264_align_up_u32((uint32_t)height,
					      RTK264_DIM_ALIGNMENT);
	if (rtk264_mul_size(aligned_width, aligned_height, &pixels) < 0 ||
	    pixels > SIZE_MAX - pixels / 2U) {
		return RTK264_ERR_ARGUMENT;
	}
	staging_size = pixels + pixels / 2U;
	if (staging_size > INT32_MAX) {
		return RTK264_ERR_ARGUMENT;
	}

	if (rtk264_mul_size((size_t)width, (size_t)height, &pixels) < 0 ||
	    pixels > SIZE_MAX - pixels / 2U) {
		return RTK264_ERR_ARGUMENT;
	}
	input_size = pixels + pixels / 2U;

	rtk264_release_encoder();
	g_rtk264.staging = rtk264_alloc_aligned(staging_size,
						 &g_rtk264.staging_allocation);
	if (g_rtk264.staging == NULL) {
		return RTK264_ERR_MEMORY;
	}
	g_rtk264.output = rtk264_alloc_aligned(staging_size,
					      &g_rtk264.output_allocation);
	if (g_rtk264.output == NULL) {
		rtk264_release_encoder();
		return RTK264_ERR_MEMORY;
	}

	/* External-frame encoding needs the same setup as the SDK UVC example. */
	hal_video_sys_init_rtl8195bhp();
	hal_enc_hw_init();
	init_h1v6_parm();
	g_rtk264.hardware_initialized = 1;

	/* h264_open() does not check pvPortMalloc() before memset(). */
	g_rtk264.encoder = pvPortMalloc(sizeof(*g_rtk264.encoder));
	if (g_rtk264.encoder == NULL) {
		rtk264_release_encoder();
		return RTK264_ERR_MEMORY;
	}
	memset(g_rtk264.encoder, 0, sizeof(*g_rtk264.encoder));
	h264_init_param(&g_rtk264.encoder->h264_parm);

	memset(&parameter, 0, sizeof(parameter));
	h264_init_param(&parameter);
	parameter.inputtype = 1; /* YUV420 semi-planar (NV12). */
	parameter.width = width;
	parameter.height = height;
	parameter.ratenum = fps;
	parameter.bps = RTK264_DEFAULT_BITRATE;
	parameter.gopLen = RTK264_DEFAULT_GOP;
	parameter.rcMode = RC_MODE_H264ABR;
	parameter.proile = H264_HIGH_PROFILE;
	parameter.idrHeader = 0; /* Copy cached SPS/PPS into each IDR. */
	parameter.level = H264ENCODER_LEVEL_4_2;

	ret = h264_set_parm(g_rtk264.encoder, &parameter);
	if (ret < 0) {
		rtk264_release_encoder();
		return RTK264_ERR_HW_INIT;
	}
	ret = h264_init_encoder(g_rtk264.encoder);
	if (ret < 0) {
		/* The SDK's h264_initial() frees its context on every error path. */
		g_rtk264.encoder = NULL;
		rtk264_release_encoder();
		return RTK264_ERR_HW_INIT;
	}

	g_rtk264.width = (uint32_t)width;
	g_rtk264.height = (uint32_t)height;
	g_rtk264.aligned_width = aligned_width;
	g_rtk264.aligned_height = aligned_height;
	g_rtk264.staging_size = staging_size;
	g_rtk264.output_capacity = staging_size;
	g_rtk264.input_size = input_size;
	rtk264_gdma_init();
	/* Self-test traffic is diagnostic and must not pollute runtime statistics. */
	g_rtk264.y_gdma.dma_ops = 0U;
	g_rtk264.y_gdma.dma_bytes = 0U;
	g_rtk264.initialized = 1;
	return RTK264_OK;
}

static int rtk264_encode_planes_ex(int type,
				   const uint8_t *src_y,
				   const uint8_t *src_u,
				   const uint8_t *src_v,
				   size_t y_stride,
				   size_t chroma_stride,
				   uint8_t *bitstream,
				   size_t bitstream_capacity,
				   size_t *bitstream_size)
{
	isp_buf_t input_buffer;
	VIDEO_BUFFER output_buffer;
	int ret;

	if (bitstream_size != NULL) {
		*bitstream_size = 0U;
	}
	if (!g_rtk264.initialized || g_rtk264.encoder == NULL) {
		return RTK264_ERR_STATE;
	}
	if (src_y == NULL || src_u == NULL || src_v == NULL ||
	    bitstream == NULL || bitstream_size == NULL ||
	    y_stride < g_rtk264.width ||
	    chroma_stride < g_rtk264.width / 2U ||
	    bitstream_capacity == 0U ||
	    type < RTK264_TYPE_AUTO || type > RTK264_TYPE_P) {
		return RTK264_ERR_ARGUMENT;
	}
	if (g_rtk264.encoding) {
		return RTK264_ERR_BUSY;
	}

	g_rtk264.encoding = 1;
	rtk264_copy_i420_to_nv12(src_y, src_u, src_v,
				 y_stride, chroma_stride);
	dcache_clean_by_addr((uint32_t *)g_rtk264.staging,
			    (int32_t)g_rtk264.staging_size);

	if (type == RTK264_TYPE_IDR || type == RTK264_TYPE_I) {
		h264_set_force_iframe(g_rtk264.encoder);
	}

	memset(&input_buffer, 0, sizeof(input_buffer));
	input_buffer.y_addr = (uint32_t)g_rtk264.staging;
	input_buffer.uv_addr = (uint32_t)(g_rtk264.staging +
		(size_t)g_rtk264.aligned_width * g_rtk264.aligned_height);

	output_buffer.output_buffer = g_rtk264.output;
	output_buffer.output_buffer_size = (uint32_t)g_rtk264.output_capacity;
	output_buffer.output_size = 0U;
	ret = h264_encode_frame(g_rtk264.encoder, &input_buffer, &output_buffer);

	if (ret == H264_OUTPUT_BUFFER_OVERFLOW) {
		ret = RTK264_ERR_OUTPUT_SMALL;
		goto encode_done;
	}
	if (ret < 0) {
		ret = RTK264_ERR_HW_ENCODE;
		goto encode_done;
	}
	if (output_buffer.output_size > g_rtk264.output_capacity) {
		ret = RTK264_ERR_HW_ENCODE;
		goto encode_done;
	}
	if (output_buffer.output_size != 0U) {
		dcache_invalidate_by_addr((uint32_t *)g_rtk264.output,
					  (int32_t)output_buffer.output_size);
	}
	*bitstream_size = output_buffer.output_size;
	if (output_buffer.output_size > bitstream_capacity) {
		ret = RTK264_ERR_OUTPUT_SMALL;
		goto encode_done;
	}

	rtk264_copy_bitstream(bitstream, g_rtk264.output,
			      output_buffer.output_size);
	ret = RTK264_OK;

encode_done:
	/* Keep the singleton busy until its shared output buffer is no longer used. */
	g_rtk264.encoding = 0;
	return ret;
}

int lib_rtk264_encode_ex(int type,
			 const uint8_t *i420,
			 size_t input_size,
			 uint8_t *bitstream,
			 size_t bitstream_capacity,
			 size_t *bitstream_size)
{
	const uint8_t *src_u;
	const uint8_t *src_v;
	size_t luma_size;
	size_t chroma_size;

	if (bitstream_size != NULL) {
		*bitstream_size = 0U;
	}
	if (!g_rtk264.initialized) {
		return RTK264_ERR_STATE;
	}
	if (i420 == NULL || input_size < g_rtk264.input_size) {
		return RTK264_ERR_ARGUMENT;
	}

	luma_size = (size_t)g_rtk264.width * g_rtk264.height;
	chroma_size = luma_size / 4U;
	src_u = i420 + luma_size;
	src_v = src_u + chroma_size;

	return rtk264_encode_planes_ex(type, i420, src_u, src_v,
				       g_rtk264.width, g_rtk264.width / 2U,
				       bitstream, bitstream_capacity,
				       bitstream_size);
}

int lib_rtk264_encode(int type, const uint8_t *i420, uint8_t *bitstream)
{
	size_t output_size = 0U;
	int ret;

	if (!g_rtk264.initialized) {
		return RTK264_ERR_STATE;
	}
	ret = lib_rtk264_encode_ex(type, i420, g_rtk264.input_size,
				    bitstream, g_rtk264.output_capacity,
				    &output_size);
	if (ret < 0) {
		return ret;
	}
	if (output_size > INT_MAX) {
		return RTK264_ERR_HW_ENCODE;
	}
	return (int)output_size;
}

void lib_rtk264_exit(void)
{
	if (!g_rtk264.encoding) {
		rtk264_release_encoder();
	}
}

int lib_x264_init(int width, int height, int fps)
{
	return lib_rtk264_init(width, height, fps);
}

int lib_x264_encode(int type, const uint8_t *i420, uint8_t *bitstream)
{
	const uint8_t *src_u;
	const uint8_t *src_v;
	size_t padded_luma_size;
	size_t output_size = 0U;
	int ret;

	if (!g_rtk264.initialized) {
		return RTK264_ERR_STATE;
	}
	if (i420 == NULL) {
		return RTK264_ERR_ARGUMENT;
	}

	/* The original x264 bridge consumes planar I420 padded to 16 pixels. */
	padded_luma_size = (size_t)g_rtk264.aligned_width *
			   g_rtk264.aligned_height;
	src_u = i420 + padded_luma_size;
	src_v = src_u + padded_luma_size / 4U;
	ret = rtk264_encode_planes_ex(type, i420, src_u, src_v,
				      g_rtk264.aligned_width,
				      g_rtk264.aligned_width / 2U,
				      bitstream,
				      g_rtk264.output_capacity,
				      &output_size);
	if (ret < 0) {
		return ret;
	}
	if (output_size > INT_MAX) {
		return RTK264_ERR_HW_ENCODE;
	}
	return (int)output_size;
}

void lib_x264_exit(void)
{
	lib_rtk264_exit();
}
