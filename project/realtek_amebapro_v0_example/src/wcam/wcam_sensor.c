#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "hal_cache.h"
#include "isp_api.h"
#include "wcam_pattern.h"
#include "wcam_uvc_control.h"
#include "wcam_sensor.h"

#define GC2053_ID_HIGH 0x20
#define GC2053_ID_LOW  0x53
#define SENSOR_BUFFERS 3

static isp_stream_t *stream;
static QueueHandle_t ready_queue, free_queue;
static void *allocations[SENSOR_BUFFERS];
static volatile uint32_t dropped_frames, overflow_frames;
static int video_initialized;

/* The SDK invokes this callback from the ISP interrupt. Exchange a free DMA
 * buffer before handing the completed one to the encoder task. */
static void frame_complete(void *unused)
{
    BaseType_t wake = pdFALSE;
    uint8_t *next, *completed;
    isp_buf_t buffer;
    (void)unused;
    if (stream->info.isp_overflow_flag) {
        stream->info.isp_overflow_flag = 0;
        ++overflow_frames;
        isp_handle_buffer(stream, NULL, MODE_SKIP);
        return;
    }
    if (xQueueReceiveFromISR(free_queue, &next, &wake) != pdTRUE) {
        ++dropped_frames;
        isp_handle_buffer(stream, NULL, MODE_SKIP);
        return;
    }
    memset(&buffer, 0, sizeof(buffer));
    buffer.y_addr = (uint32_t)next;
    buffer.uv_addr = buffer.y_addr + WCAM_Y_SIZE;
    isp_handle_buffer(stream, &buffer, MODE_EXCHANGE);
    completed = (uint8_t *)buffer.y_addr;
    if (xQueueSendFromISR(ready_queue, &completed, &wake) != pdTRUE) {
        ++dropped_frames;
        xQueueSendFromISR(free_queue, &completed, &wake);
    }
    if (wake) taskYIELD();
}

int wcam_sensor_init(void)
{
    isp_init_cfg_t init;
    isp_cfg_t cfg;
    isp_buf_t buffer;
    unsigned i;
    int id_high, id_low;

    ready_queue = xQueueCreate(SENSOR_BUFFERS, sizeof(uint8_t *));
    free_queue = xQueueCreate(SENSOR_BUFFERS, sizeof(uint8_t *));
    if (!ready_queue || !free_queue) goto failed;
    for (i = 0; i < SENSOR_BUFFERS; ++i) {
        uintptr_t aligned;
        uint8_t *frame;
        allocations[i] = malloc(WCAM_NV12_SIZE + 31);
        if (!allocations[i]) goto failed;
        aligned = ((uintptr_t)allocations[i] + 31u) & ~(uintptr_t)31u;
        frame = (uint8_t *)aligned;
        dcache_clean_invalidate_by_addr((uint32_t *)frame, WCAM_NV12_SIZE);
        if (i && xQueueSend(free_queue, &frame, 0) != pdTRUE) goto failed;
    }

    memset(&init, 0, sizeof(init));
    init.pin_idx = ISP_PIN_SEL_S1;
    init.clk = ISP_FREQ_27;
    init.ldc = LDC_OFF;
    init.fps = WCAM_FPS;
    init.isp_fw_location = 0; /* SDK flash ISP image: isp_gc2053.bin. */
    printf("[wcam][sensor] initialize GC2053 ISP S1 clock=27MHz NV12=%ux%u/%u\r\n",
           WCAM_WIDTH, WCAM_HEIGHT, WCAM_FPS);
    if (video_subsys_init(&init) < 0) {
        printf("[wcam][sensor] video_subsys_init failed\r\n");
        goto failed;
    }
    video_initialized = 1;

    memset(&cfg, 0, sizeof(cfg));
    cfg.isp_id = 0;
    cfg.format = ISP_FORMAT_YUV420_SEMIPLANAR;
    cfg.width = WCAM_WIDTH;
    cfg.height = WCAM_HEIGHT;
    cfg.fps = WCAM_FPS;
    cfg.hw_slot_num = 1;
    stream = isp_stream_create(&cfg);
    if (!stream) {
        printf("[wcam][sensor] isp_stream_create failed\r\n");
        goto failed;
    }
    memset(&buffer, 0, sizeof(buffer));
    buffer.y_addr = (uint32_t)(((uintptr_t)allocations[0] + 31u) & ~(uintptr_t)31u);
    buffer.uv_addr = buffer.y_addr + WCAM_Y_SIZE;
    isp_handle_buffer(stream, &buffer, MODE_SETUP);
    isp_stream_set_complete_callback(stream, frame_complete, NULL);
    isp_stream_apply(stream);
    isp_stream_start(stream);
    vTaskDelay(pdMS_TO_TICKS(100));
    id_high = isp_i2c_read_byte(0xF0);
    id_low = isp_i2c_read_byte(0xF1);
    printf("[wcam][sensor] chip_id=%02x%02x expected=2053 stream=%d\r\n",
           id_high, id_low, isp_stream_get_status(0));
    if (id_high != GC2053_ID_HIGH || id_low != GC2053_ID_LOW ||
        isp_stream_get_status(0) != 1) goto failed;
    return 0;

failed:
    wcam_sensor_deinit();
    return -1;
}

void wcam_sensor_deinit(void)
{
    unsigned i;
    if (stream) {
        isp_stream_stop(stream);
        isp_stream_cancel(stream);
        stream = isp_stream_destroy(stream);
    }
    if (video_initialized) {
        video_subsys_deinit(NULL);
        video_initialized = 0;
    }
    for (i = 0; i < SENSOR_BUFFERS; ++i) {
        free(allocations[i]);
        allocations[i] = NULL;
    }
    if (free_queue) { vQueueDelete(free_queue); free_queue = NULL; }
    if (ready_queue) { vQueueDelete(ready_queue); ready_queue = NULL; }
}

void wcam_sensor_discard_ready(void)
{
    uint8_t *frame;
    while (xQueueReceive(ready_queue, &frame, 0) == pdTRUE)
        wcam_sensor_release_frame(frame);
}

int wcam_sensor_get_frame(uint8_t **frame, uint32_t timeout_ms)
{
    uint8_t *newest;
    if (xQueueReceive(ready_queue, &newest, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
        return -1;
    while (xQueueReceive(ready_queue, frame, 0) == pdTRUE) {
        xQueueSend(free_queue, &newest, portMAX_DELAY);
        newest = *frame;
    }
    *frame = newest;
    dcache_invalidate_by_addr((uint32_t *)newest, WCAM_NV12_SIZE);
    return 0;
}

void wcam_sensor_release_frame(uint8_t *frame)
{
    xQueueSend(free_queue, &frame, portMAX_DELAY);
}

uint32_t wcam_sensor_drops(void)
{
    return dropped_frames + overflow_frames;
}
