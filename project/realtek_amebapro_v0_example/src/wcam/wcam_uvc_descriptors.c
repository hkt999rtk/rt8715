/* Single-mode descriptors for the original Realtek UVC device driver. */
#include "uvc/inc/usbd_uvc_desc.h"
#include "wcam_pattern.h"
#include "wcam_uvc_control.h"

/* Keep the SDK's declared C type; only the first format byte is on the wire. */
struct UVC_INPUT_HEADER_DESCRIPTOR(1, 4) uvc_input_header = {
    .bLength = UVC_DT_INPUT_HEADER_SIZE(1, 1),
    .bDescriptorType = USB_DT_CS_INTERFACE,
    .bDescriptorSubType = UVC_VS_INPUT_HEADER,
    .bNumFormats = 1,
    .bTerminalLink = 4,
    .bControlSize = 1
};

static struct uvc_format_framebased wcam_h264_format = {
    .bLength = UVC_DT_FORMAT_FRAMEBASED_SIZE,
    .bDescriptorType = USB_DT_CS_INTERFACE,
    .bDescriptorSubType = UVC_VS_FORMAT_FRAME_BASED,
    .bFormatIndex = 1,
    .bNumFrameDescriptors = 1,
    .guidFormat = {'H','2','6','4',0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71},
    .bBitsPerPixel = 0,
    .bDefaultFrameIndex = 1,
    .bVariableSize = 1
};

static struct UVC_FRAME_FRAMEBASED(3) wcam_h264_frame = {
    .bLength = UVC_DT_FRAME_FRAMEBASED_SIZE(1),
    .bDescriptorType = USB_DT_CS_INTERFACE,
    .bDescriptorSubType = UVC_VS_FRAME_FRAME_BASED,
    .bFrameIndex = 1,
    .wWidth = cpu_to_le16(WCAM_WIDTH),
    .wHeight = cpu_to_le16(WCAM_HEIGHT),
    .dwMinBitRate = cpu_to_le32(WCAM_BITRATE),
    .dwMaxBitRate = cpu_to_le32(WCAM_BITRATE),
    .dwDefaultFrameInterval = cpu_to_le32(WCAM_FRAME_INTERVAL),
    .bFrameIntervalType = 1,
    .dwBytesPerLine = 0,
    .dwFrameInterval = {cpu_to_le32(WCAM_FRAME_INTERVAL), 0, 0}
};

#define VS_DESCRIPTORS(type) \
    (type *)&uvc_input_header, (type *)&wcam_h264_format, \
    (type *)&wcam_h264_frame, (type *)&uvc_color_matching

struct uvc_descriptor_header *uvc_fs_streaming_cls[] = {
    VS_DESCRIPTORS(struct uvc_descriptor_header), NULL
};
struct uvc_descriptor_header *uvc_hs_streaming_cls[] = {
    VS_DESCRIPTORS(struct uvc_descriptor_header), NULL
};

#define CONFIG_DESCRIPTORS(ep) \
    (struct usb_descriptor_header *)&uvc_iad, \
    (struct usb_descriptor_header *)&uvc_control_intf, \
    (struct usb_descriptor_header *)&uvc_control_header, \
    (struct usb_descriptor_header *)&uvc_camera_terminal, \
    (struct usb_descriptor_header *)&uvc_processing, \
    (struct usb_descriptor_header *)&uvc_extension_unit, \
    (struct usb_descriptor_header *)&uvc_output_terminal, \
    (struct usb_descriptor_header *)&uvc_control_ep, \
    (struct usb_descriptor_header *)&uvc_control_cs_ep, \
    (struct usb_descriptor_header *)&uvc_streaming_intf_alt0, \
    VS_DESCRIPTORS(struct usb_descriptor_header), \
    (struct usb_descriptor_header *)&uvc_streaming_intf_alt1, \
    (struct usb_descriptor_header *)&ep, NULL

struct usb_descriptor_header *usbd_uvc_descriptors_FS[] = {
    CONFIG_DESCRIPTORS(uvc_fs_streaming_ep)
};
struct usb_descriptor_header *usbd_uvc_descriptors_HS[] = {
    CONFIG_DESCRIPTORS(uvc_hs_streaming_ep)
};

void wcam_uvc_descriptors_init(void)
{
    /* No ISP exists in this application, so advertise no sensor controls. */
    memset(uvc_camera_terminal.bmControls, 0, sizeof(uvc_camera_terminal.bmControls));
    memset(uvc_processing.bmControls, 0, sizeof(uvc_processing.bmControls));
    uvc_extension_unit.bNumControls = 0;
    memset(uvc_extension_unit.bmControls, 0, sizeof(uvc_extension_unit.bmControls));
    uvc_control_header.bcdUVC = cpu_to_le16(0x0110);
    uvc_color_matching.bColorPrimaries = 4;
    uvc_color_matching.bTransferCharacteristics = 1;
    uvc_color_matching.bMatrixCoefficients = 4;
}
