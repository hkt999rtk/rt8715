#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "hal_cache.h"
#include "h264_api.h"
#include "wcam_config.h"
#include "wcam_pattern.h"
#if WCAM_TX_SENSOR
#include "wcam_sensor.h"
#endif
#include "wcam_uvc_control.h"
#include "wcam_pacer.h"
#include "wcam_stats.h"
#include "wcam_net.h"
extern void hal_video_sys_init_rtl8195bhp(void);
extern void hal_video_sys_deinit_rtl8195bhp(void);
extern void hal_enc_hw_init(void);
extern void hal_enc_hw_deinit(void);
extern void init_h1v6_parm(void);
static void *aligned_buffer(size_t size,void **allocation) {
    *allocation=malloc(size+31);if(!*allocation)return NULL;
    return (void*)(((uintptr_t)*allocation+31)&~(uintptr_t)31);
}
static void tx_task(void *arg) {
    void *raw_allocation=NULL,*encoded_allocation=NULL;
    uint8_t *raw,*encoded;
    struct h264_context *encoder = NULL;
    struct h264_parameter params;
#if !WCAM_TX_SENSOR
    struct wcam_pacer pacer={0,0};
    int hardware_initialized = 0;
#endif
    struct wcam_stats stats;
    const char *stage="allocate";int result=0,first_frame=1,pending_idr=0;
    uint32_t generation = 0;
    int need_first_idr = 1;
    uint32_t counter=0,last_log=0,last_idr=0; (void)arg;
    wcam_stats_reset(&stats,xTaskGetTickCount());
    raw = NULL;
#if !WCAM_TX_SENSOR
    raw = aligned_buffer(WCAM_NV12_SIZE, &raw_allocation);
#endif
    encoded = aligned_buffer(WCAM_ENCODED_SIZE, &encoded_allocation);
    if ((!WCAM_TX_SENSOR && !raw) || !encoded) {
        printf("[wcam][error] allocation raw=%p encoded=%p\r\n", raw, encoded);
        result = -1;
        goto failed;
    }
#if WCAM_TX_SENSOR
    stage = "GC2053 ISP";
    if (wcam_sensor_init() < 0) { result = -1; goto failed; }
    printf("[wcam][stage] sensor GC2053 encoded=%p\r\n", encoded);
#else
    printf("[wcam][stage] pattern raw=%p encoded=%p\r\n", raw, encoded);
    wcam_pattern_init_role(raw,WCAM_PATTERN_TX);
    dcache_clean_by_addr((uint32_t *)raw, WCAM_NV12_SIZE);
    /* Same encoder-only initialization used by the SDK's USB camera example.
     * No isp_init, isp_stream_start or sensor initialization is called. */
    printf("[wcam][stage] video clocks\r\n");
    hal_video_sys_init_rtl8195bhp();
    printf("[wcam][stage] encoder hardware\r\n");
    hal_enc_hw_init();
    init_h1v6_parm();
    hardware_initialized = 1;
#endif
    /* h264_open() in this SDK dereferences malloc's result without checking. */
    stage = "encoder context";
    encoder = pvPortMalloc(sizeof(*encoder));
    if (!encoder) {
        result = -1;
        goto failed;
    }
    memset(encoder, 0, sizeof(*encoder));
    h264_init_param(&encoder->h264_parm);
    params = encoder->h264_parm;
    params.inputtype = ISP_FORMAT_YUV420_SEMIPLANAR;
    params.width = WCAM_WIDTH;
    params.height = WCAM_HEIGHT;
    params.ratenum = WCAM_FPS;
    params.bps = WCAM_BITRATE;
    params.gopLen = WCAM_FPS;
    params.rcMode = RC_MODE_H264CBR;
    params.proile = H264_BASE_PROFILE;
    params.level = H264ENCODER_LEVEL_4;
    params.idrHeader = 1; /* Native SPS/PPS on every IDR, no 64-byte copy gap. */
    params.force_drop_frame = 0;
    encoder->h264_parm = params;
    /* SDK h264_initial frees its context on failure; never free it twice. */
    stage = "encoder init";
    printf("[wcam][stage] %s\r\n", stage);
    result = h264_init_encoder(encoder);
    if (result < 0) {
        encoder = NULL;
        goto failed;
    }

    printf("[wcam][tx] ready 1280x720/30 %s RTP/UDP, GOP=30\r\n",
           WCAM_TX_SENSOR ? "GC2053" : "colorbar");
    for(;;) {
        uint32_t enc_start,enc_ticks;unsigned nal_flags;
#if !WCAM_TX_SENSOR
        TickType_t delay;
#endif
        uint8_t *frame = raw;
        uint32_t current = wcam_net_tx_generation();
        if (!current) {
            vTaskDelay(pdMS_TO_TICKS(100));
#if !WCAM_TX_SENSOR
            pacer.deadline=xTaskGetTickCount();
#endif
            continue;
        }
        if (current != generation) {
            generation = current;
            need_first_idr = 1;
#if WCAM_TX_SENSOR
            wcam_sensor_discard_ready();
#endif
        }
#if WCAM_TX_SENSOR
        if (wcam_sensor_get_frame(&frame, 1000) < 0) {
            printf("[wcam][sensor] frame timeout drops=%lu\r\n",
                   (unsigned long)wcam_sensor_drops());
            continue;
        }
        ++counter;
#else
        wcam_pattern_counter_role(raw, counter++, WCAM_PATTERN_TX);
        dcache_clean_by_addr((uint32_t *)raw, WCAM_NV12_SIZE);
#endif
        pending_idr |= wcam_net_take_idr();
        if (need_first_idr || (pending_idr &&
            xTaskGetTickCount() - last_idr >= pdMS_TO_TICKS(250))) {
            h264_set_force_iframe(encoder);
            last_idr = xTaskGetTickCount();
            pending_idr = 0;
            printf("[wcam][tx] force IDR frame=%lu\r\n", (unsigned long)(counter - 1));
        }
        dcache_clean_invalidate_by_addr((uint32_t *)encoded, WCAM_ENCODED_SIZE);
        encoder->source_addr = encoder->y_addr = (uint32_t)frame;
        encoder->uv_addr = (uint32_t)frame + WCAM_Y_SIZE;
        encoder->dest_addr = (uint32_t)encoded;
        encoder->dest_len = WCAM_ENCODED_SIZE;
        encoder->dest_actual_len = 0;
        stage = "encode";
        if (first_frame) printf("[wcam][stage] first encode frame=%lu\r\n", (unsigned long)(counter - 1));
        enc_start = xTaskGetTickCount();
        result = h264_encode(encoder);
        enc_ticks = xTaskGetTickCount() - enc_start;
#if WCAM_TX_SENSOR
        wcam_sensor_release_frame(frame);
#endif
        if (result < 0 || encoder->dest_actual_len == 0 ||
            encoder->dest_actual_len > WCAM_ENCODED_SIZE) {
            printf("[wcam][error] H264 rc=%d frame=%lu bytes=%lu elapsed_ticks=%lu\r\n", result,
                   (unsigned long)(counter - 1), (unsigned long)encoder->dest_actual_len,
                   (unsigned long)enc_ticks);
            if (result >= 0) result = -1; /* Invalid output despite SDK success. */
            goto failed;
        }
        dcache_invalidate_by_addr((uint32_t *)encoded, encoder->dest_actual_len);
        nal_flags = wcam_nal_flags(encoded, encoder->dest_actual_len);
        wcam_stats_encoded(&stats, encoder->dest_actual_len, enc_ticks, nal_flags);
        if (first_frame) {
            unsigned i, length = encoder->dest_actual_len < 16 ? encoder->dest_actual_len : 16;
            printf("[wcam][first] frame=%lu bytes=%lu nal_flags=%x head=",
                   (unsigned long)(counter - 1), (unsigned long)encoder->dest_actual_len, nal_flags);
            for (i = 0; i < length; ++i) printf("%02x", encoded[i]);
            printf("\r\n");
            first_frame = 0;
        }

        const unsigned headers_and_idr = WCAM_NAL_SPS | WCAM_NAL_PPS | WCAM_NAL_IDR;
        if (need_first_idr && (nal_flags & headers_and_idr) != headers_and_idr) {
            ++stats.dropped;
        } else if (wcam_net_send(encoded, encoder->dest_actual_len,
                                 (uint32_t)((uint64_t)enc_start * 90000 / configTICK_RATE_HZ),
                                 generation) != 0) {
            ++stats.dropped;
            pending_idr = 1;
        } else {
            need_first_idr = 0;
            wcam_stats_sent(&stats, encoder->dest_actual_len, 0);
        }
        if(xTaskGetTickCount()-last_log>=configTICK_RATE_HZ) {
            printf("[wcam][tx] next=%lu enc=%lu sent=%lu drop=%lu enc_ticks=%lu idr=%lu sps=%lu pps=%lu sensor_drop=%lu\r\n",
                (unsigned long)counter,(unsigned long)stats.encoded,(unsigned long)stats.sent,
                (unsigned long)stats.dropped,(unsigned long)enc_ticks,(unsigned long)stats.idr,
                (unsigned long)stats.sps,(unsigned long)stats.pps,
#if WCAM_TX_SENSOR
                (unsigned long)wcam_sensor_drops());
#else
                0ul);
#endif
            last_log=xTaskGetTickCount();wcam_stats_reset(&stats,last_log);
        }
#if !WCAM_TX_SENSOR
        delay=wcam_pacer_delay(&pacer,xTaskGetTickCount(),configTICK_RATE_HZ);if(delay)vTaskDelay(delay);
#endif
    }
failed:
    if (encoder) {
        /* h264_encode retains the context, including on SDK errors. */
        h264_release(encoder);
        vPortFree(encoder);
    }
#if WCAM_TX_SENSOR
    wcam_sensor_deinit();
#else
    if (hardware_initialized) {
        hal_enc_hw_deinit();
        hal_video_sys_deinit_rtl8195bhp();
    }
#endif
    free(raw_allocation);
    free(encoded_allocation);
    printf("[wcam][tx] stopped stage=%s rc=%d\r\n",stage,result);
    vTaskDelete(NULL);
}
void wcam_tx_start(void) {
    if(xTaskCreate(tx_task,"wcam_tx",2048,NULL,tskIDLE_PRIORITY+2,NULL)!=pdPASS)
        printf("[wcam][tx] task creation failed\r\n");
}
