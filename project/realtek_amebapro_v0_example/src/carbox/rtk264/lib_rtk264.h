#ifndef LIB_RTK264_H
#define LIB_RTK264_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* These values match the x264_picture_t type values used by BoxApp. */
typedef enum {
	RTK264_TYPE_AUTO = 0,
	RTK264_TYPE_IDR = 1,
	RTK264_TYPE_I = 2,
	RTK264_TYPE_P = 3
} rtk264_frame_type_t;

typedef enum {
	RTK264_OK = 0,
	RTK264_ERR_ARGUMENT = -1,
	RTK264_ERR_STATE = -2,
	RTK264_ERR_MEMORY = -3,
	RTK264_ERR_HW_INIT = -4,
	RTK264_ERR_HW_ENCODE = -5,
	RTK264_ERR_OUTPUT_SMALL = -6,
	RTK264_ERR_BUSY = -7
} rtk264_result_t;

/*
 * Initializes the single hardware encoder instance.
 *
 * Input frames are compact I420 with no stride padding. The PRO1 Hantro
 * encoder accepts widths from 132 through 1920 in multiples of 4, and even
 * heights from 96 through 4080. The frame rate must be positive.
 */
int lib_rtk264_init(int width, int height, int fps);

/*
 * Encodes one compact I420 frame into an Annex-B H.264 access unit.
 *
 * On success, returns RTK264_OK and stores the produced byte count in
 * bitstream_size. A skipped frame is reported as success with size zero.
 * The output buffer does not need special alignment. If the encoded access
 * unit exceeds bitstream_capacity, returns RTK264_ERR_OUTPUT_SMALL and stores
 * the required byte count in bitstream_size without writing to bitstream.
 * RTK264_TYPE_I and RTK264_TYPE_IDR both request a hardware IDR because the
 * PRO1 encoder exposes only a force-intra operation.
 */
int lib_rtk264_encode_ex(int type,
			 const uint8_t *i420,
			 size_t input_size,
			 uint8_t *bitstream,
			 size_t bitstream_capacity,
			 size_t *bitstream_size);

/*
 * Compatibility entry point matching lib_x264_encode(). It returns the
 * encoded byte count or a negative rtk264_result_t. Since this API has no
 * output capacity argument, new callers should use lib_rtk264_encode_ex().
 */
int lib_rtk264_encode(int type, const uint8_t *i420, uint8_t *bitstream);

void lib_rtk264_exit(void);

/*
 * ABI bridge for the existing prebuilt BoxApp Accessory.o.
 *
 * Unlike lib_rtk264_encode[_ex](), the legacy encoder input is contiguous
 * planar I420 whose width and height are both rounded up to 16 pixels. This
 * matches the original lib_x264 wrapper's x264_picture_alloc() layout.  The
 * implementation overlaps a persistent GDMA luma copy with CPU U/V-to-NV12
 * interleaving; if GDMA is unavailable or fails, it transparently rewrites
 * the complete luma plane with the CPU before encoding.
 */
int lib_x264_init(int width, int height, int fps);
int lib_x264_encode(int type, const uint8_t *i420, uint8_t *bitstream);
void lib_x264_exit(void);

#ifdef __cplusplus
}
#endif

#endif
