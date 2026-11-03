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
    /* The archived fun_get_alt always returns zero. Install on the first
     * state change as well as after bind, so a fast host cannot observe it. */
    get_private_usbd_uvc()->func.get_alt = current_alt;
}

static void usb_disconnected(struct usb_composite_dev *dev)
{
    (void)dev;
    streaming = 0;
    disconnected = 1;
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
        return;
    }
    if (event->type != UVC_EVENT_SETUP) return;
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
    if (usb_ep_queue(dev->func.config->cdev->gadget->ep0, out, 1) < 0)
        printf("[wcam] EP0 queue failed\r\n");
}

static void *aligned_buffer(size_t size, void **allocation)
{
    *allocation = malloc(size + 31);
    if (!*allocation) return NULL;
    return (void *)(((uintptr_t)*allocation + 31) & ~(uintptr_t)31);
}

static void wcam_task(void *arg)
{
    void *raw_allocation = NULL, *encoded_allocation = NULL;
    uint8_t *raw = aligned_buffer(WCAM_NV12_SIZE, &raw_allocation);
    uint8_t *encoded = aligned_buffer(WCAM_ENCODED_SIZE, &encoded_allocation);
    struct h264_context *encoder;
    struct h264_parameter params;
    struct uvc_dev *usb;
    static struct usbd_uvc_buffer first_payload;
    struct usbd_uvc_buffer *payload = &first_payload;
    uint32_t counter = 0;
    unsigned generation = 0;
    struct wcam_pacer pacer = {0, 0};
    int result;
    (void)arg;
    if (!raw || !encoded) {
        printf("[wcam] frame allocation failed\r\n");
        free(raw_allocation);
        free(encoded_allocation);
        vTaskDelete(NULL);
        return;
    }
    wcam_pattern_init(raw);
    dcache_clean_by_addr((uint32_t *)raw, WCAM_NV12_SIZE);
    /* Same encoder-only initialization used by the SDK's USB camera example.
     * No isp_init, isp_stream_start or sensor initialization is called. */
    hal_video_sys_init_rtl8195bhp();
    hal_enc_hw_init();
    init_h1v6_parm();
    /* h264_open() in this SDK dereferences malloc's result without checking. */
    encoder = calloc(1, sizeof(*encoder));
    if (!encoder) goto failed;
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
    if (h264_init_encoder(encoder) < 0) goto failed;
    wcam_uvc_control_init(&control);
    wcam_uvc_descriptors_init();
    set_uvc_string("wcam H264 color bar", "WCAM0001", 0x0100);
    _usb_init();
    result = wait_usb_ready();
    if (result != USB_INIT_OK) {
        printf("[wcam] USB not ready (%d); connect USB to host and reboot\r\n", result);
        goto failed;
    }
    if (usbd_uvc_init() < 0) goto failed;
    usb = get_private_usbd_uvc();
    printf("[wcam] ready: NV12 color bars -> H264 UVC 1920x1080 30 fps, counter=0\r\n");
    for (;;) {
        TickType_t delay;
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
            printf("[wcam] stream on, frame=%lu\r\n", (unsigned long)counter);
        }
        wcam_pattern_counter(raw, counter++);
        dcache_clean_by_addr((uint32_t *)raw, WCAM_NV12_SIZE);
        dcache_clean_invalidate_by_addr((uint32_t *)encoded, WCAM_ENCODED_SIZE);
        encoder->source_addr = encoder->y_addr = (uint32_t)raw;
        encoder->uv_addr = (uint32_t)raw + WCAM_Y_SIZE;
        encoder->dest_addr = (uint32_t)encoded;
        encoder->dest_len = WCAM_ENCODED_SIZE;
        encoder->dest_actual_len = 0;
        result = h264_encode(encoder);
        if (result < 0 || encoder->dest_actual_len == 0 ||
            encoder->dest_actual_len > WCAM_ENCODED_SIZE) {
            printf("[wcam] H264 error %d, bytes=%lu\r\n", result,
                   (unsigned long)encoder->dest_actual_len);
            goto failed;
        }
        dcache_invalidate_by_addr((uint32_t *)encoded, encoder->dest_actual_len);
        if (!streaming || disconnected || generation != stream_generation) continue;
        payload->mem = encoded;
        payload->bytesused = encoder->dest_actual_len;
        uvc_video_put_in_stream_queue(payload, usb);
        if (!rtw_down_timeout_sema(&usb->video.output_queue_sema, 2000)) {
            printf("[wcam] USB completion timeout; buffer retained, reboot required\r\n");
            goto failed;
        }
        payload = uvc_video_out_stream_queue(usb);
        if (!payload) goto failed;
        if (counter % WCAM_FPS == 0)
            printf("[wcam] frame=%lu bytes=%lu\r\n", (unsigned long)(counter - 1),
                   (unsigned long)encoder->dest_actual_len);
        delay = wcam_pacer_delay(&pacer, xTaskGetTickCount(), configTICK_RATE_HZ);
        if (delay) vTaskDelay(delay);
    }
usb_stopped:
    printf("[wcam] USB disconnected; reconnect and reboot for this POC\r\n");
failed:
    /* In particular, never recycle/free DMA memory after an unconfirmed
     * completion. Leave it owned until reset rather than corrupt a frame. */
    printf("[wcam] stopped\r\n");
    vTaskDelete(NULL);
}

void wcam_start(void)
{
    if (xTaskCreate(wcam_task, "wcam", 2048, NULL, tskIDLE_PRIORITY + 2, NULL) != pdPASS)
        printf("[wcam] task creation failed\r\n");
}
