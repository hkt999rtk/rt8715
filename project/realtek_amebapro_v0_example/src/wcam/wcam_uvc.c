#include "wcam_config.h"
#include "wcam_net.h"
#include <stdlib.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "hal_cache.h"
#include "h264_api.h"
#include "module_uvcd.h"
#include "uvc/inc/usbd_uvc_desc.h"
#include "wcam_pattern.h"
#include "wcam_uvc_control.h"
#include "wcam_pacer.h"
#include "wcam_stats.h"
#include "wcam_source.h"

/* Binary SDK ABI verified from the matching archive members. */
_Static_assert(sizeof(struct h264_parameter) == 136, "H264 parameter ABI mismatch");
_Static_assert(sizeof(struct h264_context) == 332, "H264 context ABI mismatch");
_Static_assert(offsetof(struct uvc_dev, change_parm_cb) == 564, "UVC ABI mismatch");
_Static_assert(sizeof(struct uvc_streaming_control) == WCAM_PROBE_SIZE, "UVC probe ABI mismatch");

extern void hal_video_sys_init_rtl8195bhp(void);
extern void hal_enc_hw_init(void);
extern void init_h1v6_parm(void);
extern void wcam_uvc_descriptors_init(void);
extern int __real_usb_composite_probe(struct usb_composite_driver *driver);
extern size_t xPortGetFreeHeapSizeExt(int index);

static struct uvc_format wcam_format = {
    .width = WCAM_WIDTH, .height = WCAM_HEIGHT,
    .format = FORMAT_TYPE_H264, .fps = WCAM_FPS
};
struct uvc_format *uvc_format_ptr = &wcam_format;
static struct wcam_uvc_control control;
static volatile unsigned stream_generation;
static volatile int streaming;
static volatile int disconnected;
static uint8_t request_error;
static struct wcam_trace trace;
static volatile uint32_t ctrl_setup, ctrl_set, ctrl_stalls, ctrl_queue_errors;
enum { TRACE_SETUP, TRACE_SET, TRACE_STALL, TRACE_QUEUE_ERROR, TRACE_STREAM, TRACE_DISCONNECT };

static void trace_event(unsigned kind, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    struct wcam_event event = {kind, a, b, c, d};
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    wcam_trace_push(&trace, event);
    __set_PRIMASK(mask);
}

static void trace_print(unsigned budget)
{
    struct wcam_event event;
    while (budget--) {
        uint32_t mask = __get_PRIMASK();
        int available;
        __disable_irq();
        available = wcam_trace_pop(&trace, &event);
        __set_PRIMASK(mask);
        if (!available) break;
        switch (event.kind) {
        case TRACE_SETUP:
            printf("[wcam][ctrl] setup bm=%02lx req=%02lx value=%04lx index=%04lx len=%lu\r\n",
                   (unsigned long)(event.a >> 8), (unsigned long)(event.a & 255),
                   (unsigned long)event.b, (unsigned long)event.c, (unsigned long)event.d);
            break;
        case TRACE_SET:
            printf("[wcam][ctrl] set selector=%lu fmt=%lu frame=%lu interval=%lu len=%lu -> fixed %ux%u/%u\r\n",
                   (unsigned long)event.a, (unsigned long)(event.b >> 8),
                   (unsigned long)(event.b & 255), (unsigned long)event.c, (unsigned long)event.d,
                   WCAM_WIDTH, WCAM_HEIGHT, WCAM_FPS);
            break;
        case TRACE_STALL:
            printf("[wcam][ctrl] STALL interface=%lu entity=%lu selector=%lu\r\n",
                   (unsigned long)event.a, (unsigned long)event.b, (unsigned long)event.c);
            break;
        case TRACE_QUEUE_ERROR:
            printf("[wcam][ctrl] EP0 queue error=%ld\r\n", (long)(int32_t)event.a);
            break;
        case TRACE_STREAM:
            printf("[wcam][usb] stream=%lu generation=%lu\r\n",
                   (unsigned long)event.a, (unsigned long)event.b);
            break;
        case TRACE_DISCONNECT:
            printf("[wcam][usb] disconnect\r\n");
            break;
        }
    }
}

static void diagnostics(struct wcam_stats *stats, uint32_t next_frame, int force)
{
    static uint32_t last_idle;
    uint32_t now = xTaskGetTickCount();
    struct wcam_rates r;
    if (!force && now - stats->start < configTICK_RATE_HZ) return;
    trace_print(force ? WCAM_TRACE_CAPACITY : 8);
    if (force || streaming || stats->encoded || now - last_idle >= 5u * configTICK_RATE_HZ) {
        r = wcam_stats_rates(stats, now, configTICK_RATE_HZ);
        printf("[wcam][stat] tick=%lu window_ms=%lu active=%d gen=%u next=%lu enc=%lu sent=%lu skip=%lu fps=%lu.%lu kbps=%lu size=%lu/%lu over=%lu\r\n",
               (unsigned long)now, (unsigned long)r.elapsed_ms, streaming, stream_generation,
               (unsigned long)next_frame, (unsigned long)stats->encoded,
               (unsigned long)stats->sent, (unsigned long)stats->dropped,
               (unsigned long)(r.fps10 / 10), (unsigned long)(r.fps10 % 10),
               (unsigned long)r.kbps, (unsigned long)stats->frame_min,
               (unsigned long)stats->frame_max, (unsigned long)stats->overruns);
        printf("[wcam][perf] enc_ms=%lu/%lu usb_ms=%lu/%lu nal_idr=%lu nonidr=%lu sps=%lu pps=%lu no_vcl=%lu heap=%lu/%lu ctrl=%lu/%lu/%lu/%lu trace_lost=%lu\r\n",
               (unsigned long)r.enc_avg_ms, (unsigned long)r.enc_max_ms,
               (unsigned long)r.usb_avg_ms, (unsigned long)r.usb_max_ms,
               (unsigned long)stats->idr, (unsigned long)stats->nonidr,
               (unsigned long)stats->sps, (unsigned long)stats->pps, (unsigned long)stats->no_vcl,
               (unsigned long)xPortGetFreeHeapSizeExt(0), (unsigned long)xPortGetFreeHeapSizeExt(1),
               (unsigned long)ctrl_setup, (unsigned long)ctrl_set,
               (unsigned long)ctrl_stalls, (unsigned long)ctrl_queue_errors, (unsigned long)trace.lost);
        last_idle = now;
    }
    /* Include console cost in the following window instead of hiding it. */
    wcam_stats_reset(stats, now);
}

static int current_alt(struct usb_function *function, unsigned interface)
{
    (void)function;
    return interface == UVC_INTF_STREAMING ? streaming : 0;
}

static void stream_changed(void *arg)
{
    const struct uvc_format *format = arg;
    int active = format->state != 0;
    if (active && !streaming) ++stream_generation;
    streaming = active;
    trace_event(TRACE_STREAM, active, stream_generation, 0, 0);
    /* The archived fun_get_alt always returns zero. Install on the first
     * state change as well as after bind, so a fast host cannot observe it. */
    get_private_usbd_uvc()->func.get_alt = current_alt;
}

static void usb_disconnected(struct usb_composite_dev *dev)
{
    (void)dev;
    streaming = 0;
    disconnected = 1;
    trace_event(TRACE_DISCONNECT, 0, 0, 0, 0);
}

/* usbd_uvc_init clears its entire context. Install the callback after that
 * clear and BEFORE registration can expose SET_INTERFACE to the host. */
int __wrap_usb_composite_probe(struct usb_composite_driver *driver)
{
    struct uvc_dev *dev = get_private_usbd_uvc();
    int result;
    dev->change_parm_cb = stream_changed;
    memcpy(&dev->probe, control.probe, WCAM_PROBE_SIZE);
    memcpy(&dev->commit, control.commit, WCAM_PROBE_SIZE);
    dev->video.fcc = FORMAT_TYPE_H264;
    dev->video.width = WCAM_WIDTH;
    dev->video.height = WCAM_HEIGHT;
    dev->video.imagesize = WCAM_ENCODED_SIZE;
    driver->disconnect = usb_disconnected;
    result = __real_usb_composite_probe(driver);
    dev->func.get_alt = current_alt;
    return result;
}

static void control_stall(struct uvc_dev *dev)
{
    request_error = 7; /* Invalid request. */
    ++ctrl_stalls;
    trace_event(TRACE_STALL, dev->command_interface, dev->command_entity, dev->control, 0);
    usb_ep_set_halt(dev->func.config->cdev->gadget->ep0);
}

/* Replaces only usbd_event.o, whose GET_MAX indexes uvc_formats[-1]. */
void uvc_events_process(struct uvc_dev *dev, struct uvc_req_data *event)
{
    uint8_t response[WCAM_PROBE_SIZE] = {0};
    struct usb_ctrlrequest *req = &event->req;
    struct usb_request *out = dev->control_req;
    int length = -1;
    if (event->type == UVC_EVENT_DATA) {
        if (dev->command_interface != UVC_INTF_STREAMING ||
            wcam_uvc_control_set(&control, dev->control,
                                event->uvc_data.data, event->uvc_data.length) < 0) {
            control_stall(dev);
            return;
        }
        memcpy(&dev->probe, control.probe, WCAM_PROBE_SIZE);
        memcpy(&dev->commit, control.commit, WCAM_PROBE_SIZE);
        ++ctrl_set;
        {
            const uint8_t *data = event->uvc_data.data;
            uint32_t interval = (uint32_t)data[4] | (uint32_t)data[5] << 8 |
                                (uint32_t)data[6] << 16 | (uint32_t)data[7] << 24;
            trace_event(TRACE_SET, dev->control, (uint32_t)data[2] << 8 | data[3],
                        interval, event->uvc_data.length);
        }
        return;
    }
    if (event->type != UVC_EVENT_SETUP) return;
    ++ctrl_setup;
    trace_event(TRACE_SETUP, (uint32_t)req->bRequestType << 8 | req->bRequest,
                req->wValue, req->wIndex, req->wLength);
    dev->command_interface = req->wIndex & 0xff;
    dev->command_entity = req->wIndex >> 8;
    dev->control = req->wValue >> 8;
    if ((req->bRequestType & 0x7f) != (USB_TYPE_CLASS | USB_RECIP_INTERFACE) ||
        dev->command_entity != 0 || (req->wValue & 0xff) != 0) {
        control_stall(dev);
        return;
    }
    if (dev->command_interface == UVC_INTF_STREAMING) {
        if (req->bRequest == UVC_SET_CUR && !(req->bRequestType & USB_DIR_IN) &&
            (dev->control == UVC_VS_PROBE_CONTROL || dev->control == UVC_VS_COMMIT_CONTROL) &&
            (req->wLength == 26 || req->wLength == WCAM_PROBE_SIZE)) {
            length = req->wLength;
        } else if (req->bRequestType & USB_DIR_IN) {
            length = wcam_uvc_control_get(&control, req->bRequest, dev->control,
                                         response, sizeof(response));
        }
    } else if (dev->command_interface == UVC_INTF_CONTROL &&
               dev->control == UVC_VC_REQUEST_ERROR_CODE_CONTROL &&
               (req->bRequestType & USB_DIR_IN)) {
        if (req->bRequest == UVC_GET_CUR) {
            response[0] = request_error;
            request_error = 0;
            length = 1;
        } else if (req->bRequest == UVC_GET_INFO) {
            response[0] = 1;
            length = 1;
        }
    }
    if (length < 0) {
        control_stall(dev);
        return;
    }
    out->length = (unsigned)length < req->wLength ? (unsigned)length : req->wLength;
    out->zero = (unsigned)length < req->wLength;
    memcpy(out->buf, response, out->length);
    {
        int result = usb_ep_queue(dev->func.config->cdev->gadget->ep0, out, 1);
        if (result < 0) {
            ++ctrl_queue_errors;
            trace_event(TRACE_QUEUE_ERROR, (uint32_t)result, 0, 0, 0);
        }
    }
}

static void *aligned_buffer(size_t size, void **allocation)
{
    *allocation = malloc(size + 31);
    if (!*allocation) return NULL;
    return (void *)(((uintptr_t)*allocation + 31) & ~(uintptr_t)31);
}

#if !WCAM_ROLE_RX
static void wcam_task(void *arg)
{
    void *raw_allocation = NULL, *encoded_allocation = NULL;
    uint8_t *raw, *encoded;
    struct h264_context *encoder;
    struct h264_parameter params;
    struct uvc_dev *usb;
    static struct usbd_uvc_buffer first_payload;
    struct usbd_uvc_buffer *payload = &first_payload;
    uint32_t counter = 0;
    unsigned generation = 0;
    struct wcam_pacer pacer = {0, 0};
    struct wcam_stats stats;
    const char *stage = "allocate";
    int result = 0, first_frame = 1;
    (void)arg;
    wcam_stats_reset(&stats, xTaskGetTickCount());
    printf("[wcam][boot] diag=3 build=%s %s tick_hz=%u heap=%lu/%lu\r\n",
           __DATE__, __TIME__, (unsigned)configTICK_RATE_HZ,
           (unsigned long)xPortGetFreeHeapSizeExt(0), (unsigned long)xPortGetFreeHeapSizeExt(1));
    printf("[wcam][config] NV12=%ux%u fps=%u bps=%u gop=%u baseline level=40 raw=%u out=%u payload=%u timeout_ms=2000\r\n",
           WCAM_WIDTH, WCAM_HEIGHT, WCAM_FPS, WCAM_BITRATE, WCAM_FPS,
           WCAM_NV12_SIZE, WCAM_ENCODED_SIZE, WCAM_PAYLOAD_SIZE);
    raw = aligned_buffer(WCAM_NV12_SIZE, &raw_allocation);
    encoded = aligned_buffer(WCAM_ENCODED_SIZE, &encoded_allocation);
    if (!raw || !encoded) {
        printf("[wcam][error] allocation raw=%p encoded=%p\r\n", raw, encoded);
        free(raw_allocation);
        free(encoded_allocation);
        result = -1;
        goto failed;
    }
    printf("[wcam][stage] pattern raw=%p encoded=%p\r\n", raw, encoded);
    wcam_pattern_init(raw);
    dcache_clean_by_addr((uint32_t *)raw, WCAM_NV12_SIZE);
    /* Same encoder-only initialization used by the SDK's USB camera example.
     * No isp_init, isp_stream_start or sensor initialization is called. */
    printf("[wcam][stage] video clocks\r\n");
    hal_video_sys_init_rtl8195bhp();
    printf("[wcam][stage] encoder hardware\r\n");
    hal_enc_hw_init();
    init_h1v6_parm();
    /* h264_open() in this SDK dereferences malloc's result without checking. */
    stage = "encoder context";
    encoder = calloc(1, sizeof(*encoder));
    if (!encoder) {
        result = -1;
        goto failed;
    }
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
    if (result < 0) goto failed;
    wcam_uvc_control_init(&control);
    wcam_uvc_descriptors_init();
    set_uvc_string("wcam H264 color bar", "WCAM0001", 0x0100);
    stage = "USB ready";
    printf("[wcam][stage] USB init/wait\r\n");
    _usb_init();
    result = wait_usb_ready();
    if (result != USB_INIT_OK) {
        printf("[wcam] USB not ready (%d); connect USB to host and reboot\r\n", result);
        goto failed;
    }
    stage = "UVC init";
    printf("[wcam][stage] %s\r\n", stage);
    result = usbd_uvc_init();
    if (result < 0) goto failed;
    usb = get_private_usbd_uvc();
    if (usb->func.config && usb->func.config->cdev)
        printf("[wcam][usb] speed_enum=%d (1=low,2=full,3=high)\r\n",
               usb->func.config->cdev->gadget->speed);
    printf("[wcam] ready: NV12 color bars -> H264 UVC %ux%u %u fps, counter=0\r\n",
           WCAM_WIDTH, WCAM_HEIGHT, WCAM_FPS);
    for (;;) {
        TickType_t delay;
        uint32_t enc_start, enc_ticks, usb_start, usb_ticks;
        unsigned nal_flags;
        diagnostics(&stats, counter, 0);
        if (disconnected) goto usb_stopped;
        if (!streaming) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (generation != stream_generation) {
            generation = stream_generation;
            h264_set_force_iframe(encoder);
            pacer.deadline = xTaskGetTickCount();
            pacer.fraction = 0;
            diagnostics(&stats, counter, 1);
            wcam_stats_reset(&stats, xTaskGetTickCount());
            first_frame = 1;
            printf("[wcam] stream on, frame=%lu speed_enum=%d generation=%u\r\n",
                   (unsigned long)counter, usb->func.config->cdev->gadget->speed, generation);
        }
        wcam_pattern_counter(raw, counter++);
        dcache_clean_by_addr((uint32_t *)raw, WCAM_NV12_SIZE);
        dcache_clean_invalidate_by_addr((uint32_t *)encoded, WCAM_ENCODED_SIZE);
        encoder->source_addr = encoder->y_addr = (uint32_t)raw;
        encoder->uv_addr = (uint32_t)raw + WCAM_Y_SIZE;
        encoder->dest_addr = (uint32_t)encoded;
        encoder->dest_len = WCAM_ENCODED_SIZE;
        encoder->dest_actual_len = 0;
        stage = "encode";
        if (first_frame) printf("[wcam][stage] first encode frame=%lu\r\n", (unsigned long)(counter - 1));
        enc_start = xTaskGetTickCount();
        result = h264_encode(encoder);
        enc_ticks = xTaskGetTickCount() - enc_start;
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
        if (!streaming || disconnected || generation != stream_generation) {
            ++stats.dropped;
            continue;
        }
        payload->mem = encoded;
        payload->bytesused = encoder->dest_actual_len;
        stage = "USB completion";
        usb_start = xTaskGetTickCount();
        uvc_video_put_in_stream_queue(payload, usb);
        if (!rtw_down_timeout_sema(&usb->video.output_queue_sema, 2000)) {
            printf("[wcam][error] USB timeout frame=%lu bytes=%lu wait_ticks=%lu; buffer retained, reboot required\r\n",
                   (unsigned long)(counter - 1), (unsigned long)encoder->dest_actual_len,
                   (unsigned long)(xTaskGetTickCount() - usb_start));
            result = -1;
            goto failed;
        }
        payload = uvc_video_out_stream_queue(usb);
        if (!payload) {
            stage = "USB empty completion queue";
            result = -1;
            goto failed;
        }
        usb_ticks = xTaskGetTickCount() - usb_start;
        wcam_stats_sent(&stats, encoder->dest_actual_len, usb_ticks);
        delay = wcam_pacer_delay(&pacer, xTaskGetTickCount(), configTICK_RATE_HZ);
        if (!delay) ++stats.overruns;
        if (delay) vTaskDelay(delay);
    }
usb_stopped:
    stage = "USB disconnected";
    result = 0;
    printf("[wcam] USB disconnected; reconnect and reboot for this POC\r\n");
failed:
    /* In particular, never recycle/free DMA memory after an unconfirmed
     * completion. Leave it owned until reset rather than corrupt a frame. */
    diagnostics(&stats, counter, 1);
    printf("[wcam][stop] stage=%s rc=%d next=%lu active=%d disconnected=%d generation=%u/%u\r\n",
           stage, result, (unsigned long)counter, streaming, disconnected, generation, stream_generation);
    vTaskDelete(NULL);
}

#endif

#if WCAM_ROLE_RX
struct rx_local_encoder {
    void *raw_allocation, *encoded_allocation;
    uint8_t *raw, *encoded;
    struct h264_context *encoder;
    uint32_t counter;
    struct wcam_pacer pacer;
};

static int rx_local_init(struct rx_local_encoder *local)
{
    struct h264_parameter params;
    memset(local, 0, sizeof(*local));
    local->raw = aligned_buffer(WCAM_NV12_SIZE, &local->raw_allocation);
    local->encoded = aligned_buffer(WCAM_ENCODED_SIZE, &local->encoded_allocation);
    local->encoder = calloc(1, sizeof(*local->encoder));
    if (!local->raw || !local->encoded || !local->encoder) return -1;
    wcam_pattern_init_role(local->raw, WCAM_PATTERN_RX);
    dcache_clean_by_addr((uint32_t *)local->raw, WCAM_NV12_SIZE);
    hal_video_sys_init_rtl8195bhp();
    hal_enc_hw_init();
    init_h1v6_parm();
    h264_init_param(&local->encoder->h264_parm);
    params = local->encoder->h264_parm;
    params.inputtype = ISP_FORMAT_YUV420_SEMIPLANAR;
    params.width = WCAM_WIDTH;
    params.height = WCAM_HEIGHT;
    params.ratenum = WCAM_FPS;
    params.bps = WCAM_BITRATE;
    params.gopLen = WCAM_FPS;
    params.rcMode = RC_MODE_H264CBR;
    params.proile = H264_BASE_PROFILE;
    params.level = H264ENCODER_LEVEL_4;
    params.idrHeader = 1;
    params.force_drop_frame = 0;
    local->encoder->h264_parm = params;
    return h264_init_encoder(local->encoder);
}

static int rx_local_encode(struct rx_local_encoder *local, size_t *length,
                           uint32_t *elapsed)
{
    uint32_t start;
    struct h264_context *encoder = local->encoder;
    wcam_pattern_counter_role(local->raw, local->counter++, WCAM_PATTERN_RX);
    dcache_clean_by_addr((uint32_t *)local->raw, WCAM_NV12_SIZE);
    dcache_clean_invalidate_by_addr((uint32_t *)local->encoded, WCAM_ENCODED_SIZE);
    encoder->source_addr = encoder->y_addr = (uint32_t)local->raw;
    encoder->uv_addr = (uint32_t)local->raw + WCAM_Y_SIZE;
    encoder->dest_addr = (uint32_t)local->encoded;
    encoder->dest_len = WCAM_ENCODED_SIZE;
    encoder->dest_actual_len = 0;
    start = xTaskGetTickCount();
    if (h264_encode(encoder) < 0) return -1;
    *elapsed = xTaskGetTickCount() - start;
    *length = encoder->dest_actual_len;
    if (!*length || *length > WCAM_ENCODED_SIZE) return -1;
    dcache_invalidate_by_addr((uint32_t *)local->encoded, *length);
    return 0;
}

/* The SDK's uvc_video_put_in_stream_queue takes input_lock and appends
 * buffer_list to input_queue. The mutex can block, so validate the USB
 * session after acquiring it, at the point where ownership changes. */
static int rx_queue_payload(struct uvc_dev *usb, struct usbd_uvc_buffer *payload,
                            uint32_t generation, struct wcam_source *source,
                            const struct wcam_net_frame *frame, unsigned flags)
{
    uint32_t mask;
    uint64_t now = 0;
    int queued;

    rtw_mutex_get(&usb->video.input_lock);
    taskENTER_CRITICAL();
    if (frame) now = wcam_net_now_ms();
    /* FreeRTOS masks the tick/preemption with BASEPRI. Also exclude USB
     * callbacks of any interrupt priority through the actual list insertion. */
    mask = __get_PRIMASK();
    __disable_irq();
    queued = wcam_session_current(generation, stream_generation, streaming, disconnected);
    if (queued && frame) {
        queued = frame->generation == generation &&
                 wcam_source_accept(source, (flags & 0xe) == 0xe, frame->received_ms, now);
    }
    if (queued) list_add_tail(&payload->buffer_list, &usb->video.input_queue);
    __set_PRIMASK(mask);
    taskEXIT_CRITICAL();
    rtw_mutex_put(&usb->video.input_lock);
    return queued;
}

static void wcam_rx_task(void *arg) {
    struct uvc_dev *usb;static struct usbd_uvc_buffer first_payload;
    struct usbd_uvc_buffer *payload=&first_payload;
    struct wcam_net_frame frame;struct wcam_stats stats;
    struct rx_local_encoder local;struct wcam_source source;
    uint32_t generation=0;int result;
    (void)arg;wcam_stats_reset(&stats,xTaskGetTickCount());
    printf("[wcam][rx] initialize black waiting screen\r\n");
    result=rx_local_init(&local);if(result<0)goto failed;
    wcam_source_start(&source,wcam_net_now_ms());
    wcam_uvc_control_init(&control);wcam_uvc_descriptors_init();
    set_uvc_string("wcam H264 wireless RX","WCAMRX01",0x0200);
    printf("[wcam][rx] USB init/wait\r\n");_usb_init();
    result=wait_usb_ready();if(result!=USB_INIT_OK)goto failed;
    result=usbd_uvc_init();if(result<0)goto failed;usb=get_private_usbd_uvc();
    printf("[wcam][rx] ready: black waiting screen -> TX RTP when IDR arrives, H264 UVC 1280x720/30\r\n");
    for(;;) {
        int remote_frame=0,got_frame=0;
        size_t length;uint32_t enc_ticks=0,start;
        unsigned flags;
        diagnostics(&stats,local.counter,0);
        if(disconnected) {result=-1;goto failed;}
        if(!streaming) {
            generation=0;wcam_net_rx_active(0);vTaskDelay(pdMS_TO_TICKS(10));continue;
        }
        if(generation!=stream_generation) {
            generation=stream_generation;
            wcam_source_start(&source,wcam_net_now_ms());
            wcam_net_rx_active(generation);
            h264_set_force_iframe(local.encoder);
            local.pacer.deadline=xTaskGetTickCount();local.pacer.fraction=0;
            printf("[wcam][rx] UVC start generation=%lu, request fresh IDR\r\n",(unsigned long)generation);
        }
        unsigned wait_ms = 0;
        if (source.remote) {
            uint64_t age = wcam_net_now_ms() - source.last_remote_ms;
            if (age < WCAM_REMOTE_TIMEOUT_MS) {
                wait_ms = WCAM_REMOTE_TIMEOUT_MS - (unsigned)age;
                if (wait_ms > 10) wait_ms = 10;
            }
        }
        got_frame = wcam_net_frame_get(&frame, wait_ms);
        if (got_frame) {
            int was_remote = source.remote;
            if (frame.generation == generation) {
                flags = wcam_nal_flags(frame.data, frame.length);
                payload->mem = frame.data;
                payload->bytesused = frame.length;
                /* Receiver writes CPU memory; clean it before USB DMA reads. */
                dcache_clean_by_addr((uint32_t *)frame.data, frame.length);
                start = xTaskGetTickCount();
                remote_frame = rx_queue_payload(usb, payload, generation, &source, &frame, flags);
            }
            if (remote_frame) {
                length = frame.length;
                if (!was_remote)
                    printf("[wcam][rx] source=TX IDR bytes=%lu\r\n", (unsigned long)length);
            } else {
                wcam_net_frame_release(&frame);
                ++stats.dropped;
            }
        }
        if (!remote_frame) {
            if(wcam_source_expired(&source,wcam_net_now_ms())) {
                wcam_net_rx_request_idr();
                h264_set_force_iframe(local.encoder);
                local.pacer.deadline=xTaskGetTickCount();local.pacer.fraction=0;
                printf("[wcam][rx] remote timeout; source=waiting screen, request FAST_UPDATE\r\n");
            }
            if (source.remote || !wcam_session_current(generation, stream_generation,
                                                      streaming, disconnected)) continue;
            if (rx_local_encode(&local, &length, &enc_ticks) < 0) {
                printf("[wcam][rx] local H264 encode failed frame=%lu\r\n",
                       (unsigned long)(local.counter - 1));
                result = -1;
                goto failed;
            }
            payload->mem = local.encoded;
            payload->bytesused = length;
            flags = wcam_nal_flags(local.encoded, length);
            start = xTaskGetTickCount();
            if (!rx_queue_payload(usb, payload, generation, &source, NULL, flags)) {
                ++stats.dropped;
                continue;
            }
        }
        wcam_stats_encoded(&stats, length, enc_ticks, flags);
        if(!rtw_down_timeout_sema(&usb->video.output_queue_sema,2000)) {
            printf("[wcam][rx] USB timeout: DMA frame retained until reboot\r\n");result=-1;goto failed;
        }
        payload=uvc_video_out_stream_queue(usb);
        if(!payload) {result=-1;goto failed;}
        wcam_stats_sent(&stats,length,xTaskGetTickCount()-start);
        if(remote_frame) wcam_net_frame_release(&frame);
        else {
            TickType_t delay=wcam_pacer_delay(&local.pacer,xTaskGetTickCount(),configTICK_RATE_HZ);
            if(!delay) ++stats.overruns;
            if(delay) vTaskDelay(delay);
        }
    }
failed:
    wcam_net_rx_active(0);printf("[wcam][rx] stopped rc=%d; reboot required\r\n",result);vTaskDelete(NULL);
}
#endif

void wcam_start(void)
{
    if (xTaskCreate(
#if WCAM_ROLE_RX
        wcam_rx_task,
#else
        wcam_task,
#endif
        "wcam", 2048, NULL, tskIDLE_PRIORITY + 2, NULL) != pdPASS)
        printf("[wcam] task creation failed\r\n");
}
