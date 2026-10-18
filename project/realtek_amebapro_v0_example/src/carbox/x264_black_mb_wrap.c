/* Fast path for the single IDR generated for the CarPlay loading screen.
 * The wrapper uses offsets audited against customer lib_x264.a SHA-256
 * 2957d3dcbb949b60ec1f6f79095ed09e2ddee4a6c94d687100fa1395afc05dfa.
 * The firmware makefile rejects other archives until their ABI is audited.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "hal_timer.h"

#if UINTPTR_MAX != UINT32_MAX
#error "The customer x264 ABI requires 32-bit pointers"
#endif

enum {
    X264_PARAM_THREADS = 0x004,
    X264_PARAM_WIDTH = 0x010,
    X264_PARAM_HEIGHT = 0x014,
    X264_PARAM_INTRA_MODES = 0x164,
    X264_SLICE_TYPE = 0x14a0,
    X264_MB_COUNT = 0x1e60,
    X264_MB_INDEX = 0x1e78,
    X264_MB_CBP_TABLE = 0x1f48,
    X264_MB_TYPE = 0x2094,
    X264_MB_TRANSFORM_8X8 = 0x20a0,
    X264_MB_CBP_LUMA = 0x20a4,
    X264_MB_CBP_CHROMA = 0x20a8,
    X264_MB_I16_PRED_MODE = 0x20ac,
    X264_MB_CHROMA_PRED_MODE = 0x20b0,
    X264_MB_FENC_Y = 0x2f88,
    X264_MB_FENC_U = 0x2f8c,
    X264_MB_FENC_V = 0x2f90,
    X264_MB_FDEC_Y = 0x2fa0,
    X264_MB_FDEC_U = 0x2fa4,
    X264_MB_FDEC_V = 0x2fa8,
    X264_MB_CACHE_NNZ = 0x3670,
    X264_MB_LOSSLESS = 0x3c44,
    X264_PRED_16X16 = 0x4b8c,
    X264_PRED_8X8C = 0x4ba8,
    X264_PIXF_MBCMP_16X16 = 0x4ca8,
    X264_PIXF_MBCMP_8X8 = 0x4cb4,
    X264_PIXF_INTRA_X3_16X16 = 0x4e08,
    X264_PIXF_INTRA_X3_8X8C = 0x4e14,
    X264_MB_I16 = 2,
    X264_SLICE_I = 2,
    X264_FENC_STRIDE = 16,
    X264_FDEC_STRIDE = 32
};

extern void __real_x264_macroblock_analyse(void *encoder);
extern void __real_x264_macroblock_encode(void *encoder);
extern void __real_x264_macroblock_write_cabac(void *encoder, void *cabac);

typedef int (*pixel_cmp_t)(uint8_t *, int, uint8_t *, int);

typedef struct {
    uint32_t analyse_us, encode_us, cabac_us;
    uint32_t analyse_black_us, encode_black_us, cabac_black_us;
    uint32_t first_us;
    uint32_t luma_equal, luma_fallback, chroma_equal, chroma_fallback;
    uint32_t merged_luma, merged_chroma;
} black_profile_t;

static black_profile_t profile;
static pixel_cmp_t original_luma_cmp, original_chroma_cmp;

static uint32_t read_u32(const uint8_t *base, unsigned offset)
{
    uint32_t value;
    memcpy(&value, base + offset, sizeof(value));
    return value;
}

static void write_u32(uint8_t *base, unsigned offset, uint32_t value)
{
    memcpy(base + offset, &value, sizeof(value));
}

/* x264_macroblock_cache_load() has already copied this MB into 16-byte
 * stride fenc scratch planes when the wrapper is entered. */
typedef struct {
    uint8_t y, u, v;
    int valid;
} black_sample_t;

typedef struct {
    const uint8_t *y, *u, *v;
} mb_planes_t;

static black_sample_t black_sample;
static mb_planes_t active_fenc, active_fdec;
static unsigned analysed_index;
static int analysed_black;
static int analysis_in_progress;

static mb_planes_t mb_planes(const uint8_t *encoder)
{
    mb_planes_t p;
    p.y = (const uint8_t *)(uintptr_t)read_u32(encoder, X264_MB_FENC_Y);
    p.u = (const uint8_t *)(uintptr_t)read_u32(encoder, X264_MB_FENC_U);
    p.v = (const uint8_t *)(uintptr_t)read_u32(encoder, X264_MB_FENC_V);
    return p;
}

static int plane_is_constant(const uint8_t *plane, unsigned stride,
                             unsigned width, unsigned height, uint8_t value)
{
    const uint32_t repeated = 0x01010101U * value;
    unsigned y, x;
    /* Both callers use 8- or 16-pixel rows. Four-pixel compares avoid
     * revisiting 384 individual bytes on every known-black macroblock. */
    for (y = 0; y < height; ++y)
        for (x = 0; x < width; x += 4) {
            uint32_t word;
            memcpy(&word, plane + y * stride + x, sizeof(word));
            if (word != repeated)
                return 0;
        }
    return 1;
}

/* A pixel difference metric is exactly zero for equal images. Keep the
 * customer's comparator for every non-identical prediction, so mode scores
 * and tie-breaking remain those of the original encoder. */
static int black_mbcmp_16x16(uint8_t *a, int a_stride,
                             uint8_t *b, int b_stride)
{
    /* The input was checked once for the whole MB. Only read the predicted
     * plane here; confirm the exact pointers/strides before using that fact. */
    if (a == active_fdec.y && b == active_fenc.y &&
        a_stride == X264_FDEC_STRIDE && b_stride == X264_FENC_STRIDE &&
        plane_is_constant(a, X264_FDEC_STRIDE, 16, 16, black_sample.y)) {
        ++profile.luma_equal;
        return 0;
    }
    ++profile.luma_fallback;
    return original_luma_cmp(a, a_stride, b, b_stride);
}

static int black_mbcmp_8x8(uint8_t *a, int a_stride,
                           uint8_t *b, int b_stride)
{
    if (a_stride == X264_FDEC_STRIDE && b_stride == X264_FENC_STRIDE &&
        ((a == active_fdec.u && b == active_fenc.u &&
          plane_is_constant(a, X264_FDEC_STRIDE, 8, 8, black_sample.u)) ||
         (a == active_fdec.v && b == active_fenc.v &&
          plane_is_constant(a, X264_FDEC_STRIDE, 8, 8, black_sample.v)))) {
        ++profile.chroma_equal;
        return 0;
    }
    ++profile.chroma_fallback;
    return original_chroma_cmp(a, a_stride, b, b_stride);
}

static int is_black_mb(mb_planes_t p, black_sample_t sample)
{
    return sample.valid && p.y && p.u && p.v &&
        plane_is_constant(p.y, X264_FENC_STRIDE, 16, 16, sample.y) &&
        plane_is_constant(p.u, X264_FENC_STRIDE, 8, 8, sample.u) &&
        plane_is_constant(p.v, X264_FENC_STRIDE, 8, 8, sample.v);
}

/* x264's scan8 positions for this MB. Keep the top/left cache positions:
 * CABAC uses their nonzero values to select contexts for later blocks. */
static const uint8_t current_scan8[27] = {
    12, 13, 20, 21, 14, 15, 22, 23,
    28, 29, 36, 37, 30, 31, 38, 39,
    9, 10, 17, 18, 33, 34, 41, 42,
    44, 45, 46
};

/* The original I16 encode predicts Y, U and V before calculating residuals.
 * If all predicted samples equal the known-black input, DCT and quantization
 * would see only zeroes. Leave reconstruction and CABAC to x264's normal
 * downstream path, with exactly the state produced by zero coefficients. */
static int encode_exact_black_without_residual(uint8_t *h, uint32_t index)
{
    typedef void (*predict_t)(uint8_t *);
    const uint32_t luma_mode = read_u32(h, X264_MB_I16_PRED_MODE);
    const uint32_t chroma_mode = read_u32(h, X264_MB_CHROMA_PRED_MODE);
    const uint32_t luma_fn = luma_mode <= 6 ?
        read_u32(h, X264_PRED_16X16 + luma_mode * 4) : 0;
    const uint32_t chroma_fn = chroma_mode <= 6 ?
        read_u32(h, X264_PRED_8X8C + chroma_mode * 4) : 0;
    uint8_t *fdec_y = (uint8_t *)(uintptr_t)read_u32(h, X264_MB_FDEC_Y);
    uint8_t *fdec_u = (uint8_t *)(uintptr_t)read_u32(h, X264_MB_FDEC_U);
    uint8_t *fdec_v = (uint8_t *)(uintptr_t)read_u32(h, X264_MB_FDEC_V);
    int16_t *cbp = (int16_t *)(uintptr_t)read_u32(h, X264_MB_CBP_TABLE);
    unsigned i;

    if (read_u32(h, X264_MB_TYPE) != X264_MB_I16 ||
        read_u32(h, X264_MB_LOSSLESS) || !luma_fn || !chroma_fn ||
        !fdec_y || !fdec_u || !fdec_v || !cbp)
        return 0;

    ((predict_t)(uintptr_t)luma_fn)(fdec_y);
    ((predict_t)(uintptr_t)chroma_fn)(fdec_u);
    ((predict_t)(uintptr_t)chroma_fn)(fdec_v);
    if (!plane_is_constant(fdec_y, X264_FDEC_STRIDE, 16, 16, black_sample.y) ||
        !plane_is_constant(fdec_u, X264_FDEC_STRIDE, 8, 8, black_sample.u) ||
        !plane_is_constant(fdec_v, X264_FDEC_STRIDE, 8, 8, black_sample.v))
        return 0;

    write_u32(h, X264_MB_TRANSFORM_8X8, 0);
    write_u32(h, X264_MB_CBP_LUMA, 0);
    write_u32(h, X264_MB_CBP_CHROMA, 0);
    for (i = 0; i < sizeof(current_scan8); ++i)
        h[X264_MB_CACHE_NNZ + current_scan8[i]] = 0;
    cbp[index] = 0;
    return 1;
}

void __wrap_x264_macroblock_analyse(void *encoder)
{
    static unsigned black_count;
    static unsigned mb_count;
    uint8_t *h = (uint8_t *)encoder;
    uint32_t width, height, index, total, original_modes;
    mb_planes_t p;
    int black, hooked = 0;
    uint32_t start_us, elapsed_us;
    uint32_t old_luma_cmp = 0, old_chroma_cmp = 0;

    if (!h || read_u32(h, X264_SLICE_TYPE) != X264_SLICE_I ||
        read_u32(h, X264_PARAM_THREADS) != 1) {
        __real_x264_macroblock_analyse(encoder);
        return;
    }
    width = read_u32(h, X264_PARAM_WIDTH);
    height = read_u32(h, X264_PARAM_HEIGHT);
    /* Restrict this closed-library ABI hook to the two tested UI sizes. */
    if (width != 2560 || (height != 1200 && height != 1440)) {
        __real_x264_macroblock_analyse(encoder);
        return;
    }
    total = read_u32(h, X264_MB_COUNT);
    index = read_u32(h, X264_MB_INDEX);
    if (total != (width / 16) * (height / 16) || index >= total) {
        __real_x264_macroblock_analyse(encoder);
        return;
    }
    p = mb_planes(h);
    if (index == 0) {
        unsigned i;
        memset(&profile, 0, sizeof(profile));
        black_count = mb_count = 0;
        black_sample.valid = 0;
        if (p.y && p.u && p.v) {
            /* The first eight pixels are known black in this UI. I420
             * shares one U/V sample between each horizontal pixel pair. */
            black_sample.y = p.y[0];
            black_sample.u = p.u[0];
            black_sample.v = p.v[0];
            black_sample.valid = black_sample.y <= 32 &&
                black_sample.u >= 127 && black_sample.u <= 129 &&
                black_sample.v >= 127 && black_sample.v <= 129;
            for (i = 0; i < 8; ++i)
                if (p.y[i] != black_sample.y)
                    black_sample.valid = 0;
            for (i = 0; i < 4; ++i)
                if (p.u[i] != black_sample.u || p.v[i] != black_sample.v)
                    black_sample.valid = 0;
            printf("[X264 BLACK] first row Y=%d,%d,%d,%d,%d,%d,%d,%d "
                   "U=%d,%d,%d,%d V=%d,%d,%d,%d candidate=%d/%d/%d enabled=%d\r\n",
                   p.y[0], p.y[1], p.y[2], p.y[3],
                   p.y[4], p.y[5], p.y[6], p.y[7],
                   p.u[0], p.u[1], p.u[2], p.u[3],
                   p.v[0], p.v[1], p.v[2], p.v[3],
                   black_sample.y, black_sample.u, black_sample.v,
                   black_sample.valid);
        } else {
            printf("[X264 BLACK] first row YUV unavailable; fast path disabled\r\n");
        }
    }

    black = is_black_mb(p, black_sample);
    analysed_index = index;
    analysed_black = black;
    original_modes = read_u32(h, X264_PARAM_INTRA_MODES);
    if (black) {
        write_u32(h, X264_PARAM_INTRA_MODES, 0); /* I16x16 only for this MB */
        active_fenc = p;
        active_fdec.y = (const uint8_t *)(uintptr_t)read_u32(h, X264_MB_FDEC_Y);
        active_fdec.u = (const uint8_t *)(uintptr_t)read_u32(h, X264_MB_FDEC_U);
        active_fdec.v = (const uint8_t *)(uintptr_t)read_u32(h, X264_MB_FDEC_V);
        old_luma_cmp = read_u32(h, X264_PIXF_MBCMP_16X16);
        old_chroma_cmp = read_u32(h, X264_PIXF_MBCMP_8X8);
        if (old_luma_cmp && old_chroma_cmp &&
            old_luma_cmp != (uint32_t)(uintptr_t)black_mbcmp_16x16 &&
            old_chroma_cmp != (uint32_t)(uintptr_t)black_mbcmp_8x8) {
            original_luma_cmp = (pixel_cmp_t)(uintptr_t)old_luma_cmp;
            original_chroma_cmp = (pixel_cmp_t)(uintptr_t)old_chroma_cmp;
            write_u32(h, X264_PIXF_MBCMP_16X16,
                      (uint32_t)(uintptr_t)black_mbcmp_16x16);
            write_u32(h, X264_PIXF_MBCMP_8X8,
                      (uint32_t)(uintptr_t)black_mbcmp_8x8);
            hooked = 1;
        }
        if (index == 0) {
            profile.merged_luma = !!read_u32(h, X264_PIXF_INTRA_X3_16X16);
            profile.merged_chroma = !!read_u32(h, X264_PIXF_INTRA_X3_8X8C);
        }
    }
    start_us = hal_read_curtime_us();
    if (index == 0)
        profile.first_us = start_us;
    analysis_in_progress = 1;
    __real_x264_macroblock_analyse(encoder);
    analysis_in_progress = 0;
    elapsed_us = hal_read_curtime_us() - start_us;
    profile.analyse_us += elapsed_us;
    if (black)
        profile.analyse_black_us += elapsed_us;
    if (hooked) {
        write_u32(h, X264_PIXF_MBCMP_16X16, old_luma_cmp);
        write_u32(h, X264_PIXF_MBCMP_8X8, old_chroma_cmp);
    }
    if (black)
        write_u32(h, X264_PARAM_INTRA_MODES, original_modes);

    black_count += (unsigned)black;
    ++mb_count;
    if (index == total - 1) {
        printf("[X264 BLACK] %ux%u eligible=%u/%u (%u%%)\r\n",
               (unsigned)width, (unsigned)height, black_count, mb_count,
               mb_count ? 100 * black_count / mb_count : 0);
    }
}

/* Inspect the reconstructed MB after either the original encoder or the
 * exact-prediction shortcut. The statistics also show how often the shortcut
 * was taken and retain the previous zero-residual safety check. */
void __wrap_x264_macroblock_encode(void *encoder)
{
    static unsigned black_count, i16_count, cbp0_count, exact_count;
    static unsigned luma_exact_count, chroma_exact_count;
    static unsigned fast_count;
    static int mismatch_sampled;
    uint8_t *h = (uint8_t *)encoder;
    uint32_t width, height, index, total;
    uint32_t start_us, elapsed_us;
    int black, fast;
    int16_t *cbp;

    if (!h || analysis_in_progress ||
        read_u32(h, X264_SLICE_TYPE) != X264_SLICE_I ||
        read_u32(h, X264_PARAM_THREADS) != 1) {
        __real_x264_macroblock_encode(encoder);
        return;
    }
    width = read_u32(h, X264_PARAM_WIDTH);
    height = read_u32(h, X264_PARAM_HEIGHT);
    total = read_u32(h, X264_MB_COUNT);
    index = read_u32(h, X264_MB_INDEX);
    if (width != 2560 || (height != 1200 && height != 1440) ||
        total != (width / 16) * (height / 16) || index >= total) {
        __real_x264_macroblock_encode(encoder);
        return;
    }
    if (index == 0)
    {
        black_count = i16_count = cbp0_count = exact_count = 0;
        luma_exact_count = chroma_exact_count = 0;
        fast_count = 0;
        mismatch_sampled = 0;
    }
    black = black_sample.valid && analysed_index == index && analysed_black;
    start_us = hal_read_curtime_us();
    fast = black && encode_exact_black_without_residual(h, index);
    if (fast)
        ++fast_count;
    else
        __real_x264_macroblock_encode(encoder);
    elapsed_us = hal_read_curtime_us() - start_us;
    profile.encode_us += elapsed_us;
    if (black)
        profile.encode_black_us += elapsed_us;

    if (black) {
        ++black_count;
        if (read_u32(h, X264_MB_TYPE) == X264_MB_I16) {
            ++i16_count;
            cbp = (int16_t *)(uintptr_t)read_u32(h, X264_MB_CBP_TABLE);
            if (cbp && cbp[index] == 0) {
                ++cbp0_count;
                if (fast) {
                    /* The fast path just checked every predicted Y/U/V pixel. */
                    ++luma_exact_count;
                    ++chroma_exact_count;
                    ++exact_count;
                } else {
                    mb_planes_t fdec;
                    int y_exact, u_exact, v_exact;
                    fdec.y = (const uint8_t *)(uintptr_t)read_u32(h, X264_MB_FDEC_Y);
                    fdec.u = (const uint8_t *)(uintptr_t)read_u32(h, X264_MB_FDEC_U);
                    fdec.v = (const uint8_t *)(uintptr_t)read_u32(h, X264_MB_FDEC_V);
                    y_exact = fdec.y && plane_is_constant(fdec.y, X264_FDEC_STRIDE,
                                                           16, 16, black_sample.y);
                    u_exact = fdec.u && plane_is_constant(fdec.u, X264_FDEC_STRIDE,
                                                           8, 8, black_sample.u);
                    v_exact = fdec.v && plane_is_constant(fdec.v, X264_FDEC_STRIDE,
                                                           8, 8, black_sample.v);
                    luma_exact_count += (unsigned)y_exact;
                    chroma_exact_count += (unsigned)(u_exact && v_exact);
                    if (y_exact && u_exact && v_exact)
                        ++exact_count;
                    else if (!mismatch_sampled && fdec.y && fdec.u && fdec.v) {
                        mismatch_sampled = 1;
                        printf("[X264 BLACK] mismatch mb=%u Y=%u,%u,%u,%u/%u,%u,%u,%u "
                               "U=%u,%u/%u,%u V=%u,%u/%u,%u target=%u/%u/%u\r\n",
                               (unsigned)index, fdec.y[0], fdec.y[1],
                               fdec.y[2], fdec.y[3], fdec.y[X264_FDEC_STRIDE],
                               fdec.y[X264_FDEC_STRIDE + 1],
                               fdec.y[X264_FDEC_STRIDE + 2],
                               fdec.y[X264_FDEC_STRIDE + 3],
                               fdec.u[0], fdec.u[1], fdec.u[X264_FDEC_STRIDE],
                               fdec.u[X264_FDEC_STRIDE + 1], fdec.v[0], fdec.v[1],
                               fdec.v[X264_FDEC_STRIDE],
                               fdec.v[X264_FDEC_STRIDE + 1], black_sample.y,
                               black_sample.u, black_sample.v);
                    }
                }
            }
        }
    }
    if (index == total - 1)
        printf("[X264 BLACK] zero-residual candidates black=%u i16=%u "
               "cbp0=%u exact=%u luma_exact=%u chroma_exact=%u fast=%u\r\n",
               black_count, i16_count, cbp0_count, exact_count,
               luma_exact_count, chroma_exact_count, fast_count);
}

void __wrap_x264_macroblock_write_cabac(void *encoder, void *cabac)
{
    uint8_t *h = (uint8_t *)encoder;
    uint32_t width, height, index, total, start_us, elapsed_us;
    int black;

    if (!h || analysis_in_progress ||
        read_u32(h, X264_SLICE_TYPE) != X264_SLICE_I ||
        read_u32(h, X264_PARAM_THREADS) != 1) {
        __real_x264_macroblock_write_cabac(encoder, cabac);
        return;
    }
    width = read_u32(h, X264_PARAM_WIDTH);
    height = read_u32(h, X264_PARAM_HEIGHT);
    total = read_u32(h, X264_MB_COUNT);
    index = read_u32(h, X264_MB_INDEX);
    if (width != 2560 || (height != 1200 && height != 1440) ||
        total != (width / 16) * (height / 16) || index >= total) {
        __real_x264_macroblock_write_cabac(encoder, cabac);
        return;
    }
    black = black_sample.valid && analysed_index == index && analysed_black;
    start_us = hal_read_curtime_us();
    __real_x264_macroblock_write_cabac(encoder, cabac);
    elapsed_us = hal_read_curtime_us() - start_us;
    profile.cabac_us += elapsed_us;
    if (black)
        profile.cabac_black_us += elapsed_us;
    if (index == total - 1)
        printf("[X264 TIME] mb_wall=%u us analyse=%u (black=%u) "
               "encode=%u (black=%u) cabac=%u (black=%u)\r\n",
               (unsigned)(hal_read_curtime_us() - profile.first_us),
               (unsigned)profile.analyse_us, (unsigned)profile.analyse_black_us,
               (unsigned)profile.encode_us, (unsigned)profile.encode_black_us,
               (unsigned)profile.cabac_us, (unsigned)profile.cabac_black_us);
    if (index == total - 1)
        printf("[X264 MODE] luma_equal=%u fallback=%u chroma_equal=%u "
               "fallback=%u merged=%u/%u\r\n",
               (unsigned)profile.luma_equal, (unsigned)profile.luma_fallback,
               (unsigned)profile.chroma_equal,
               (unsigned)profile.chroma_fallback,
               (unsigned)profile.merged_luma,
               (unsigned)profile.merged_chroma);
}
