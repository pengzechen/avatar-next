#include "uvc_video.h"

#include "klog.h"
#include "string.h"
#include "timer/timer.h"
#include "uapi/avatar_uvc.h"
#include "usb/uvc.h"

#define UVC_ERR_INVAL  22
#define UVC_ERR_NODEV  19
#define UVC_ERR_NOSPC  28
#define UVC_ERR_NOSYS  38
#define UVC_ERR_IO      5
typedef struct {
    bool present;
    bool streaming;
    uint32_t dev_addr;
    usb_device_info_t dev;
    uint32_t sequence;
    uint64_t last_capture_ms;
} uvc_video_state_t;

static uvc_video_state_t g_uvc_video0;

static void uvc_copy_cstr(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0)
        return;

    size_t i = 0;
    while (i + 1 < dst_len && src && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

void uvc_video_clear(void)
{
    memset(&g_uvc_video0, 0, sizeof(g_uvc_video0));
}

int uvc_video_register_device(uint32_t dev_addr, const usb_device_info_t *dev)
{
    if (!dev || !dev->is_uvc)
        return -UVC_ERR_INVAL;

    g_uvc_video0.present = true;
    g_uvc_video0.dev_addr = dev_addr;
    g_uvc_video0.dev = *dev;
    g_uvc_video0.sequence = 0;
    g_uvc_video0.streaming = true;
    g_uvc_video0.last_capture_ms = timer_get_uptime_ms();

    KLOG_INFO("[UVC] registered /dev/video0: addr=%lu VID=0x%04x PID=0x%04x %ux%u MJPEG ep=%u alt=%u\n",
              (unsigned long)dev_addr,
              dev->id_vendor, dev->id_product,
              dev->uvc_frame_width, dev->uvc_frame_height,
              dev->uvc_ep_num, dev->uvc_alt_setting);
    return 0;
}

static int uvc_video_capture(usb_uvc_frame_t *frame)
{
    if (!frame)
        return -UVC_ERR_INVAL;
    if (!g_uvc_video0.present)
        return -UVC_ERR_NODEV;

    if (!g_uvc_video0.streaming) {
        int restart_rc = uvc_restart_video_stream(g_uvc_video0.dev_addr,
                                                  g_uvc_video0.dev.b_max_packet_size0,
                                                  &g_uvc_video0.dev);
        if (restart_rc != 0) {
            g_uvc_video0.streaming = false;
            return -UVC_ERR_IO;
        }
        g_uvc_video0.streaming = true;
    }

    int rc = uvc_capture_one_frame(g_uvc_video0.dev_addr, &g_uvc_video0.dev, frame);
    if (rc != 0)
        return -UVC_ERR_IO;

    g_uvc_video0.last_capture_ms = timer_get_uptime_ms();
    return 0;
}

int uvc_video_read_frame(void *buf, size_t len)
{
    if (!buf || len == 0)
        return -UVC_ERR_INVAL;

    usb_uvc_frame_t frame;
    int rc = uvc_video_capture(&frame);
    if (rc != 0)
        return rc;
    if (len < frame.length)
        return -UVC_ERR_NOSPC;

    memcpy(buf, frame.data, frame.length);
    g_uvc_video0.sequence++;
    return (int)frame.length;
}

static int uvc_video_querycap(struct avatar_uvc_cap *cap)
{
    if (!cap)
        return -UVC_ERR_INVAL;
    if (!g_uvc_video0.present)
        return -UVC_ERR_NODEV;

    memset(cap, 0, sizeof(*cap));
    uvc_copy_cstr(cap->driver, sizeof(cap->driver), "avatar-uvc");
    uvc_copy_cstr(cap->card, sizeof(cap->card), "UVC Camera");
    uvc_copy_cstr(cap->bus_info, sizeof(cap->bus_info), "usb-dwc2");
    cap->version = 1;
    cap->capabilities = AV_UVC_CAP_VIDEO_CAPTURE |
                        AV_UVC_CAP_READWRITE |
                        AV_UVC_CAP_STREAMING;
    cap->device_caps = cap->capabilities;
    cap->vid = g_uvc_video0.dev.id_vendor;
    cap->pid = g_uvc_video0.dev.id_product;
    cap->dev_addr = (avatar_u8)g_uvc_video0.dev_addr;
    return 0;
}

static int uvc_video_get_format(struct avatar_uvc_format *fmt)
{
    if (!fmt)
        return -UVC_ERR_INVAL;
    if (!g_uvc_video0.present)
        return -UVC_ERR_NODEV;

    const usb_device_info_t *dev = &g_uvc_video0.dev;
    memset(fmt, 0, sizeof(*fmt));
    fmt->width = dev->uvc_frame_width;
    fmt->height = dev->uvc_frame_height;
    fmt->pixelformat = AV_PIXFMT_MJPEG;
    fmt->frame_interval_100ns = dev->uvc_frame_interval;
    fmt->max_frame_size = dev->uvc_negotiated_frame_size;
    fmt->max_payload_size = dev->uvc_negotiated_payload_size;
    fmt->format_index = dev->uvc_format_index;
    fmt->frame_index = dev->uvc_frame_index;
    fmt->interface_number = dev->uvc_vs_interface;
    fmt->endpoint_number = dev->uvc_ep_num;
    fmt->alt_setting = dev->uvc_alt_setting;
    return 0;
}

static int uvc_video_get_frame(struct avatar_uvc_frame *dst)
{
    if (!dst || dst->buf == 0 || dst->buf_size == 0)
        return -UVC_ERR_INVAL;

    usb_uvc_frame_t frame;
    int rc = uvc_video_capture(&frame);
    if (rc != 0)
        return rc;
    if (dst->buf_size < frame.length)
        return -UVC_ERR_NOSPC;

    void *user_buf = (void *)(uintptr_t)dst->buf;
    memcpy(user_buf, frame.data, frame.length);

    const usb_device_info_t *dev = &g_uvc_video0.dev;
    dst->bytes_used = frame.length;
    dst->sequence = g_uvc_video0.sequence++;
    dst->pixelformat = AV_PIXFMT_MJPEG;
    dst->width = dev->uvc_frame_width;
    dst->height = dev->uvc_frame_height;
    dst->transfers = frame.transfers;
    dst->data_packets = frame.data_packets;
    dst->fid = frame.fid;
    return 0;
}

static int uvc_video_streamon(void)
{
    if (!g_uvc_video0.present)
        return -UVC_ERR_NODEV;

    int rc = uvc_restart_video_stream(g_uvc_video0.dev_addr,
                                      g_uvc_video0.dev.b_max_packet_size0,
                                      &g_uvc_video0.dev);
    if (rc != 0) {
        g_uvc_video0.streaming = false;
        return -UVC_ERR_IO;
    }

    g_uvc_video0.streaming = true;
    g_uvc_video0.last_capture_ms = timer_get_uptime_ms();
    return 0;
}

static int uvc_video_streamoff(void)
{
    if (!g_uvc_video0.present)
        return -UVC_ERR_NODEV;

    int rc = uvc_stop_video_stream(g_uvc_video0.dev_addr,
                                   g_uvc_video0.dev.b_max_packet_size0,
                                   &g_uvc_video0.dev);
    if (rc != 0)
        return -UVC_ERR_IO;

    g_uvc_video0.streaming = false;
    g_uvc_video0.last_capture_ms = 0;
    return 0;
}

int uvc_video_ioctl(uint64_t req, void *argp)
{
    switch ((uint32_t)req) {
        case AV_UVC_IOC_QUERYCAP:
            return uvc_video_querycap((struct avatar_uvc_cap *)argp);
        case AV_UVC_IOC_G_FMT:
            return uvc_video_get_format((struct avatar_uvc_format *)argp);
        case AV_UVC_IOC_GET_FRAME:
            return uvc_video_get_frame((struct avatar_uvc_frame *)argp);
        case AV_UVC_IOC_STREAMON:
            return uvc_video_streamon();
        case AV_UVC_IOC_STREAMOFF:
            return uvc_video_streamoff();
        default:
            return -UVC_ERR_NOSYS;
    }
}
