#include "ui_h264_handover_diag.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "diag.h"
#include "FreeRTOS.h"
#include "task.h"

extern int __real_IsFileExist(const char *path);
extern int __real_lib_x264_encode(int type, char *input, char *output);
extern void __real_lib_accessory_send_video(const void *data, int bytes);
extern int DeleteFile(const char *path);

#define TRACE_FRAMES 40U
#define HASH_INIT 2166136261U
#define SPS_RAW_LOG_MAX 512U

typedef struct {
    const uint8_t *data;
    unsigned size, at, bits, byte, zeros;
    int valid;
} bits_t;

typedef struct {
    unsigned valid, frame_bits, poc_type, poc_bits, frame_only;
    unsigned profile, level, chroma, max_refs;
    unsigned coded_width, coded_height, width, height;
    unsigned crop_left, crop_right, crop_top, crop_bottom;
    uint32_t hash;
} sps_t;

typedef struct {
    unsigned valid, sps_id, cabac;
    uint32_t hash;
} pps_t;

typedef struct {
    sps_t sps[32];
    pps_t pps[64];
    unsigned frames, have_frame, last_frame, last_pps_id;
    unsigned latest_sps_id, latest_sps_valid;
    uint32_t sps_hash, pps_hash;
} stream_t;

typedef struct {
    unsigned nals, vcl, ref, pps_id, frame_num, poc, idr, sps, pps;
    unsigned have_frame, have_poc, anomaly, offset;
    char types[128];
} frame_t;

static stream_t x264_stream
    __attribute__((section(".lpddr.bss.ui_h264_diag")));
static stream_t sent_stream
    __attribute__((section(".lpddr.bss.ui_h264_diag")));
static stream_t forward_stream
    __attribute__((section(".lpddr.bss.ui_h264_diag")));
static stream_t iphone_stream
    __attribute__((section(".lpddr.bss.ui_h264_diag")));
static uint32_t x264_hash = HASH_INIT, sent_hash = HASH_INIT;
static uint32_t x264_bytes, sent_bytes, sent_calls;
static int cache_checked, remote_started, handoff_done, unexpected_task_logged;
static int forward_task_logged;

static uint32_t hash_update(uint32_t hash, const uint8_t *data, unsigned size)
{
    unsigned i;
    for (i = 0; i < size; ++i)
        hash = (hash ^ data[i]) * 16777619U;
    return hash;
}

/* Include the NAL header byte, but omit the Annex-B start code.  Print each
 * distinct SPS once per source so the two parameter sets can be compared. */
static void log_sps_raw(const char *source, const uint8_t *nal,
                        unsigned size, uint32_t hash)
{
    static uint32_t local_hash, iphone_hash;
    static unsigned local_seen, iphone_seen;
    uint32_t *last_hash = !strcmp(source, "local") ? &local_hash : &iphone_hash;
    unsigned *seen = !strcmp(source, "local") ? &local_seen : &iphone_seen;
    unsigned logged = size < SPS_RAW_LOG_MAX ? size : SPS_RAW_LOG_MAX;
    static const char digits[] = "0123456789abcdef";
    unsigned at;

    if (*seen && *last_hash == hash)
        return;
    *seen = 1;
    *last_hash = hash;
    for (at = 0; at < logged; at += 32U) {
        unsigned count = logged - at < 32U ? logged - at : 32U;
        char hex[32U * 2U + 1U];
        unsigned i;
        for (i = 0; i < count; ++i) {
            hex[i * 2U] = digits[nal[at + i] >> 4];
            hex[i * 2U + 1U] = digits[nal[at + i] & 15U];
        }
        hex[count * 2U] = '\0';
        rt_printf("[UIH264][SPSRAW] source=%s size=%lu hash=%08lx "
                  "at=%lu hex=%s truncated=%u\n",
                  source, (unsigned long)size, (unsigned long)hash,
                  (unsigned long)at, hex, size > SPS_RAW_LOG_MAX);
    }
}

/* RBSP bit reader: skip emulation-prevention 03 after two zero bytes. */
static unsigned get_bits(bits_t *b, unsigned count)
{
    unsigned value = 0, i;
    if (count > 32)
        b->valid = 0;
    for (i = 0; i < count && b->valid; ++i) {
        if (!b->bits) {
            if (b->zeros == 2 && b->at < b->size && b->data[b->at] == 3) {
                ++b->at;
                b->zeros = 0;
            }
            if (b->at == b->size) {
                b->valid = 0;
                break;
            }
            b->byte = b->data[b->at++];
            b->zeros = b->byte ? 0 : (b->zeros < 2 ? b->zeros + 1 : 2);
            b->bits = 8;
        }
        value = (value << 1) | ((b->byte >> --b->bits) & 1U);
    }
    return value;
}

static unsigned get_ue(bits_t *b)
{
    unsigned zeros = 0;
    while (b->valid && !get_bits(b, 1)) {
        if (++zeros > 30) {
            b->valid = 0;
            return 0;
        }
    }
    return b->valid ? (1U << zeros) - 1U + get_bits(b, zeros) : 0;
}

static int get_se(bits_t *b)
{
    unsigned value = get_ue(b);
    return (value & 1U) ? (int)((value + 1U) / 2U) : -(int)(value / 2U);
}

static void skip_scaling(bits_t *b, unsigned count)
{
    int last = 8, next = 8;
    unsigned i;
    for (i = 0; i < count && b->valid; ++i) {
        if (next)
            next = (last + get_se(b) + 256) & 255;
        last = next ? next : last;
    }
}

static void read_sps(stream_t *s, const uint8_t *nal, unsigned size, uint32_t hash)
{
    bits_t b = {nal + 1, size - 1, 0, 0, 0, 0, 1};
    unsigned profile = get_bits(&b, 8), chroma = 1, id, frame_bits, poc_type;
    unsigned poc_bits = 0, frame_only, separate = 0, i;
    unsigned level, max_refs, width_mbs, height_map, coded_width, coded_height;
    unsigned crop_left = 0, crop_right = 0, crop_top = 0, crop_bottom = 0;
    unsigned crop_x, crop_y;
    uint64_t cropped_width, cropped_height;
    sps_t *result;
    s->latest_sps_valid = 0;
    get_bits(&b, 8); /* constraints */
    level = get_bits(&b, 8);
    id = get_ue(&b);
    if (!b.valid || id >= 32)
        return;
    /* A malformed replacement must not leave the prior SPS for this ID active
     * in the diagnostic comparison. */
    s->sps[id].valid = 0;
    if (profile == 100 || profile == 110 || profile == 122 ||
        profile == 244 || profile == 44 || profile == 83 ||
        profile == 86 || profile == 118 || profile == 128 ||
        profile == 138 || profile == 139 || profile == 134 || profile == 135) {
        chroma = get_ue(&b);
        if (chroma > 3)
            return;
        if (chroma == 3)
            separate = get_bits(&b, 1);
        get_ue(&b); /* luma bit depth */
        get_ue(&b); /* chroma bit depth */
        get_bits(&b, 1); /* transform bypass */
        if (get_bits(&b, 1))
            for (i = 0; i < (chroma == 3 ? 12U : 8U); ++i)
                if (get_bits(&b, 1))
                    skip_scaling(&b, i < 6 ? 16U : 64U);
    }
    frame_bits = get_ue(&b) + 4U;
    poc_type = get_ue(&b);
    if (poc_type == 0)
        poc_bits = get_ue(&b) + 4U;
    else if (poc_type == 1) {
        unsigned cycles;
        get_bits(&b, 1);
        get_se(&b);
        get_se(&b);
        cycles = get_ue(&b);
        if (cycles > 255)
            return;
        for (i = 0; i < cycles; ++i)
            get_se(&b);
    } else if (poc_type != 2)
        return;
    max_refs = get_ue(&b);
    get_bits(&b, 1); /* gaps permitted */
    width_mbs = get_ue(&b) + 1U;
    height_map = get_ue(&b) + 1U;
    frame_only = get_bits(&b, 1);
    if (!frame_only)
        get_bits(&b, 1); /* mb_adaptive_frame_field_flag */
    get_bits(&b, 1); /* direct_8x8_inference_flag */
    if (get_bits(&b, 1)) { /* frame_cropping_flag */
        crop_left = get_ue(&b);
        crop_right = get_ue(&b);
        crop_top = get_ue(&b);
        crop_bottom = get_ue(&b);
    }
    if (!b.valid || frame_bits > 16 || poc_bits > 16 ||
        !width_mbs || width_mbs > 512 || !height_map || height_map > 512)
        return;
    coded_width = width_mbs * 16U;
    coded_height = height_map * 16U * (2U - frame_only);
    /* H.264 7.4.2.1.1: cropping units depend on chroma subsampling. */
    crop_x = chroma == 0 || separate ? 1U : chroma == 3 ? 1U : 2U;
    crop_y = (chroma == 1 && !separate ? 2U : 1U) * (2U - frame_only);
    cropped_width = ((uint64_t)crop_left + crop_right) * crop_x;
    cropped_height = ((uint64_t)crop_top + crop_bottom) * crop_y;
    if (cropped_width >= coded_width || cropped_height >= coded_height)
        return;
    result = &s->sps[id];
    result->valid = 1;
    result->frame_bits = frame_bits;
    result->poc_type = poc_type;
    result->poc_bits = poc_bits;
    result->frame_only = frame_only;
    result->profile = profile;
    result->level = level;
    result->chroma = chroma;
    result->max_refs = max_refs;
    result->coded_width = coded_width;
    result->coded_height = coded_height;
    result->width = coded_width - (unsigned)cropped_width;
    result->height = coded_height - (unsigned)cropped_height;
    result->crop_left = crop_left;
    result->crop_right = crop_right;
    result->crop_top = crop_top;
    result->crop_bottom = crop_bottom;
    result->hash = hash;
    s->latest_sps_id = id;
    s->latest_sps_valid = 1;
    s->sps_hash = hash;
}

static void read_pps(stream_t *s, const uint8_t *nal, unsigned size, uint32_t hash)
{
    bits_t b = {nal + 1, size - 1, 0, 0, 0, 0, 1};
    unsigned id = get_ue(&b), sps_id = get_ue(&b), cabac = get_bits(&b, 1);
    if (b.valid && id < 64 && sps_id < 32) {
        s->pps[id].valid = 1;
        s->pps[id].sps_id = sps_id;
        s->pps[id].cabac = cabac;
        s->pps[id].hash = hash;
        s->pps_hash = hash;
    }
}

static void read_slice(stream_t *s, frame_t *f, const uint8_t *nal,
                       unsigned size, unsigned type)
{
    bits_t b = {nal + 1, size - 1, 0, 0, 0, 0, 1};
    unsigned pps_id;
    sps_t *sps;
    get_ue(&b); /* first_mb_in_slice */
    get_ue(&b); /* slice_type */
    pps_id = get_ue(&b);
    if (!b.valid || pps_id >= 64)
        return;
    if (!f->vcl) {
        f->vcl = type;
        f->ref = (nal[0] >> 5) & 3U;
        f->pps_id = pps_id;
    }
    if (f->have_frame || !s->pps[pps_id].valid)
        return;
    sps = &s->sps[s->pps[pps_id].sps_id];
    if (!sps->valid)
        return;
    f->frame_num = get_bits(&b, sps->frame_bits);
    if (!sps->frame_only && get_bits(&b, 1))
        get_bits(&b, 1);
    if (type == 5)
        get_ue(&b); /* idr_pic_id */
    if (sps->poc_type == 0 && sps->poc_bits) {
        f->poc = get_bits(&b, sps->poc_bits);
        f->have_poc = b.valid;
    }
    f->have_frame = b.valid;
    if (b.valid && f->ref) {
        /* frame_num need not advance on a non-reference picture. */
        if (s->have_frame && type != 5 &&
            f->frame_num != ((s->last_frame + 1U) & ((1U << sps->frame_bits) - 1U)))
            f->anomaly |= 1U; /* frame_num jump */
        s->last_frame = f->frame_num;
        s->last_pps_id = pps_id;
        s->have_frame = 1;
    }
}

static unsigned prefix_size(const uint8_t *p, unsigned bytes, unsigned at)
{
    if (at + 3 > bytes || p[at] || p[at + 1])
        return 0;
    if (p[at + 2] == 1)
        return 3;
    return at + 4 <= bytes && !p[at + 2] && p[at + 3] == 1 ? 4 : 0;
}

static void inspect_nal(stream_t *s, frame_t *f, const uint8_t *nal, unsigned size)
{
    unsigned type, used;
    uint32_t hash;
    if (!size)
        return;
    type = nal[0] & 31U;
    used = strlen(f->types);
    if (used + 4 < sizeof(f->types))
        snprintf(f->types + used, sizeof(f->types) - used,
                 "%s%u", used ? "," : "", type);
    ++f->nals;
    hash = hash_update(HASH_INIT, nal, size);
    if (type == 7 && size > 1) {
        ++f->sps;
        read_sps(s, nal, size, hash);
        if (s == &sent_stream || s == &iphone_stream)
            log_sps_raw(s == &sent_stream ? "local" : "iphone",
                        nal, size, hash);
    } else if (type == 8 && size > 1) {
        ++f->pps;
        read_pps(s, nal, size, hash);
    } else if ((type == 1 || type == 5) && size > 1) {
        if (type == 5)
            ++f->idr;
        read_slice(s, f, nal, size, type);
    }
}

/* Annex-B may have an envelope; AVCC must form an exact length chain. */
static const char *inspect(stream_t *s, const uint8_t *p, unsigned bytes, frame_t *f)
{
    unsigned at = 0, prefix = 0, i, next, length;
    memset(f, 0, sizeof(*f));
    ++s->frames;
    if (!p || !bytes)
        return "empty";
    for (i = 0; i < bytes && i < 256; ++i)
        if ((prefix = prefix_size(p, bytes, i)) != 0) {
            at = i;
            break;
        }
    if (prefix) {
        f->offset = at;
        while (prefix && f->nals < 64) {
            unsigned nal_at = at + prefix;
            for (next = nal_at; next < bytes; ++next)
                if (prefix_size(p, bytes, next))
                    break;
            if (next > nal_at)
                inspect_nal(s, f, p + nal_at, next - nal_at);
            at = next;
            prefix = prefix_size(p, bytes, at);
        }
        if (at < bytes)
            f->anomaly |= 2U; /* NAL limit */
        return "annexb";
    }
    at = 0;
    while (at + 5 <= bytes && f->nals < 64) {
        length = ((unsigned)p[at] << 24) | ((unsigned)p[at + 1] << 16) |
                 ((unsigned)p[at + 2] << 8) | p[at + 3];
        if (!length || length > bytes - at - 4 || !(p[at + 4] & 31U) ||
            (p[at + 4] & 31U) > 23)
            break;
        inspect_nal(s, f, p + at + 4, length);
        at += length + 4;
    }
    if (at == bytes && f->nals)
        return "avcc";
    memset(f, 0, sizeof(*f));
    return "unknown";
}

static void log_sps(const char *source, const stream_t *stream)
{
    const sps_t *sps;
    if (!stream->latest_sps_valid) {
        rt_printf("[UIH264][SPS] source=%s parse=FAIL\n", source);
        return;
    }
    sps = &stream->sps[stream->latest_sps_id];
    rt_printf("[UIH264][SPS] source=%s parse=OK id=%lu "
              "coded=%lux%lu visible=%lux%lu crop=%lu,%lu,%lu,%lu "
              "profile=%lu level=%lu chroma=%lu refs=%lu frame_only=%lu "
              "hash=%08lx\n",
              source, (unsigned long)stream->latest_sps_id,
              (unsigned long)sps->coded_width, (unsigned long)sps->coded_height,
              (unsigned long)sps->width, (unsigned long)sps->height,
              (unsigned long)sps->crop_left, (unsigned long)sps->crop_right,
              (unsigned long)sps->crop_top, (unsigned long)sps->crop_bottom,
              (unsigned long)sps->profile, (unsigned long)sps->level,
              (unsigned long)sps->chroma, (unsigned long)sps->max_refs,
              (unsigned long)sps->frame_only, (unsigned long)sps->hash);
}

static const sps_t *sps_for_pps(const stream_t *stream, unsigned pps_id)
{
    const pps_t *pps;
    if (pps_id >= 64)
        return NULL;
    pps = &stream->pps[pps_id];
    if (!pps->valid || !stream->sps[pps->sps_id].valid)
        return NULL;
    return &stream->sps[pps->sps_id];
}

static void log_frame(const char *source, stream_t *s, const void *data, int bytes)
{
    const uint8_t *p = data;
    unsigned n = p && bytes > 0 ? (unsigned)bytes : 0;
    frame_t f;
    const char *format = inspect(s, p, n, &f);
    uint32_t hash = hash_update(HASH_INIT, p, n);
    if (f.sps && (!strcmp(source, "send") || !strcmp(source, "iphone")))
        log_sps(!strcmp(source, "send") ? "local" : "iphone", s);
    if (s->frames > TRACE_FRAMES && !f.anomaly)
        return;
    rt_printf("[UIH264] %s #%lu fmt=%s off=%lu bytes=%lu hash=%08lx "
              "nals=[%s] vcl=%lu ref=%lu pps_id=%lu fn=%ld poc=%ld "
              "sps=%lu/%08lx pps=%lu/%08lx idr=%lu anomaly=%lu "
              "head=%02x%02x%02x%02x\n",
              source, (unsigned long)s->frames, format,
              (unsigned long)f.offset, (unsigned long)n, (unsigned long)hash,
              f.types, (unsigned long)f.vcl, (unsigned long)f.ref,
              (unsigned long)f.pps_id,
              f.have_frame ? (long)f.frame_num : -1L,
              f.have_poc ? (long)f.poc : -1L,
              (unsigned long)f.sps, (unsigned long)s->sps_hash,
              (unsigned long)f.pps, (unsigned long)s->pps_hash,
              (unsigned long)f.idr, (unsigned long)f.anomaly,
              n > 0 ? p[0] : 0, n > 1 ? p[1] : 0,
              n > 2 ? p[2] : 0, n > 3 ? p[3] : 0);
}

int __wrap_IsFileExist(const char *path)
{
    size_t length;
    int existed, removed;
    if (!path || cache_checked)
        return __real_IsFileExist(path);
    length = strlen(path);
    if (length < 13 || strcmp(path + length - 13, "loading.video") != 0)
        return __real_IsFileExist(path);
    cache_checked = 1;
    existed = __real_IsFileExist(path);
    removed = existed ? DeleteFile(path) : 0;
    rt_printf("[UIH264] cache check existed=%d remove=%d rebuild=1\n",
              existed, removed);
    return 0;
}

int __wrap_lib_x264_encode(int type, char *input, char *output)
{
    int bytes = __real_lib_x264_encode(type, input, output);
    if (bytes > 0 && output) {
        x264_hash = hash_update(x264_hash, (const uint8_t *)output, (unsigned)bytes);
        x264_bytes += (unsigned)bytes;
        log_frame("x264", &x264_stream, output, bytes);
    }
    return bytes;
}

void __wrap_lib_accessory_send_video(const void *data, int bytes)
{
    const char *task = pcTaskGetName(xTaskGetCurrentTaskHandle());
    if (task && strcmp(task, "Accessory") == 0 && !remote_started) {
        if (data && bytes > 0) {
            sent_hash = hash_update(sent_hash, data, (unsigned)bytes);
            sent_bytes += (unsigned)bytes;
        }
        ++sent_calls;
        log_frame("send", &sent_stream, data, bytes);
    } else {
        if (!forward_task_logged) {
            forward_task_logged = 1;
            rt_printf("[UIH264] nonlocal send task=%s\n",
                      task ? task : "(null)");
        }
        log_frame("forward", &forward_stream, data, bytes);
    }
    __real_lib_accessory_send_video(data, bytes);
}

void carbox_ui_h264_remote_frame(const void *data, int bytes)
{
    const char *task = pcTaskGetName(xTaskGetCurrentTaskHandle());
    frame_t first;
    const sps_t *local_sps, *remote_sps;
    if (!task || strcmp(task, "AirPlayScreenReceiver") != 0) {
        if (!unexpected_task_logged) {
            unexpected_task_logged = 1;
            rt_printf("[UIH264] receiver hook task=%s bytes=%d ignored\n",
                      task ? task : "(null)", bytes);
        }
        return;
    }
    remote_started = 1;
    if (!handoff_done) {
        const char *format = inspect(&iphone_stream, data,
                                     data && bytes > 0 ? (unsigned)bytes : 0,
                                     &first);
        if (first.sps)
            log_sps("iphone", &iphone_stream);
        if (!first.vcl) {
            if (iphone_stream.frames <= TRACE_FRAMES)
                rt_printf("[UIH264] iphone pre-vcl #%lu fmt=%s bytes=%d "
                          "nals=[%s] sps=%08lx pps=%08lx\n",
                          (unsigned long)iphone_stream.frames, format,
                          bytes, first.types,
                          (unsigned long)iphone_stream.sps_hash,
                          (unsigned long)iphone_stream.pps_hash);
            return;
        }
        rt_printf("[UIH264][HANDOFF] x264=%lu/%lu/%08lx "
                  "send=%lu/%lu/%08lx exact=%d last_local_fn=%ld "
                  "local_sps=%08lx local_pps=%08lx "
                  "iphone_fmt=%s nals=[%s] first_vcl=%lu ref=%lu "
                  "pps_id=%lu idr=%lu fn=%ld poc=%ld "
                  "iphone_sps=%08lx iphone_pps=%08lx anomaly=%lu\n",
                  (unsigned long)x264_stream.frames,
                  (unsigned long)x264_bytes, (unsigned long)x264_hash,
                  (unsigned long)sent_calls, (unsigned long)sent_bytes,
                  (unsigned long)sent_hash,
                  x264_stream.frames > 0 && sent_calls > 0 &&
                  x264_bytes == sent_bytes && x264_hash == sent_hash,
                  sent_stream.have_frame ? (long)sent_stream.last_frame : -1L,
                  (unsigned long)sent_stream.sps_hash,
                  (unsigned long)sent_stream.pps_hash,
                  format, first.types, (unsigned long)first.vcl,
                  (unsigned long)first.ref, (unsigned long)first.pps_id,
                  (unsigned long)first.idr,
                  first.have_frame ? (long)first.frame_num : -1L,
                  first.have_poc ? (long)first.poc : -1L,
                  (unsigned long)iphone_stream.sps_hash,
                  (unsigned long)iphone_stream.pps_hash,
                  (unsigned long)first.anomaly);
        local_sps = sent_stream.have_frame ?
                    sps_for_pps(&sent_stream, sent_stream.last_pps_id) : NULL;
        remote_sps = sps_for_pps(&iphone_stream, first.pps_id);
        rt_printf("[UIH264][DIM] local_valid=%d local=%lux%lu coded=%lux%lu "
                  "iphone_valid=%d iphone=%lux%lu coded=%lux%lu "
                  "visible_match=%d coded_match=%d\n",
                  local_sps != NULL,
                  local_sps ? (unsigned long)local_sps->width : 0UL,
                  local_sps ? (unsigned long)local_sps->height : 0UL,
                  local_sps ? (unsigned long)local_sps->coded_width : 0UL,
                  local_sps ? (unsigned long)local_sps->coded_height : 0UL,
                  remote_sps != NULL,
                  remote_sps ? (unsigned long)remote_sps->width : 0UL,
                  remote_sps ? (unsigned long)remote_sps->height : 0UL,
                  remote_sps ? (unsigned long)remote_sps->coded_width : 0UL,
                  remote_sps ? (unsigned long)remote_sps->coded_height : 0UL,
                  local_sps && remote_sps ?
                  (int)(local_sps->width == remote_sps->width &&
                        local_sps->height == remote_sps->height) : -1,
                  local_sps && remote_sps ?
                  (int)(local_sps->coded_width == remote_sps->coded_width &&
                        local_sps->coded_height == remote_sps->coded_height) : -1);
        handoff_done = 1;
        return;
    }
    log_frame("iphone", &iphone_stream, data, bytes);
}
