/*
 * driver/usb/uvc_video_glue.c — UVC /dev/video0 薄 C 层
 *
 * 底层 USB 操作全部委托给 Rust 侧的 avatar_usb crate，
 * 本文件仅处理 pseudofs 所需的 read_frame / ioctl 接口。
 */

#include "usb/uvc_video.h"
#include "usb_api.h"
#include "uapi/avatar_uvc.h"
#include "klog.h"
#include "string.h"

#define UVC_ERR_INVAL  22
#define UVC_ERR_NODEV  19
#define UVC_ERR_NOSPC  28
#define UVC_ERR_NOSYS  38
#define UVC_ERR_IO      5

static struct {
    bool present;
    bool streaming;
    usb_enumerate_result_t enum_result;
    uint32_t sequence;
} g_uvc_state;

void uvc_video_clear(void)
{
    memset(&g_uvc_state, 0, sizeof(g_uvc_state));
}

/*
 * uvc_video_ensure_init — 延迟初始化：首次调用时从枚举结果中获取 UVC 设备信息
 *
 * 如果 Lua 层的 dwc2_usb.init() 已经完成枚举，这里直接获取结果。
 */
static int uvc_video_ensure_init(void)
{
    if (g_uvc_state.present)
        return 0;

    /* 由 Lua 层先调用 dwc2_usb.init()，枚举结果已经存在于 Rust 侧 */
    if (g_uvc_state.enum_result.has_uvc) {
        g_uvc_state.present = true;
        g_uvc_state.streaming = true;
        return 0;
    }

    return -UVC_ERR_NODEV;
}

/*
 * uvc_video_set_enum_result — 由 Lua init 路径调用，注入枚举结果
 *
 * 这让 /dev/video0 的 C 层知道 UVC 设备已就绪。
 */
void uvc_video_set_enum_result(const usb_enumerate_result_t *result)
{
    if (!result)
        return;
    g_uvc_state.enum_result = *result;
    if (result->has_uvc) {
        g_uvc_state.present = true;
        g_uvc_state.streaming = true;
    }
}

int uvc_video_read_frame(void *buf, size_t len)
{
    if (!buf || len == 0)
        return -UVC_ERR_INVAL;

    if (uvc_video_ensure_init() != 0)
        return -UVC_ERR_NODEV;

    usb_uvc_frame_t frame;
    int rc = dwc2_usb_capture_first_uvc_frame(&g_uvc_state.enum_result, &frame);
    if (rc != 0)
        return -UVC_ERR_IO;

    if (len < frame.length)
        return -UVC_ERR_NOSPC;

    memcpy(buf, frame.data, frame.length);
    g_uvc_state.sequence++;
    return (int)frame.length;
}

static int uvc_video_querycap(struct avatar_uvc_cap *cap)
{
    if (!cap)
        return -UVC_ERR_INVAL;
    if (!g_uvc_state.present)
        return -UVC_ERR_NODEV;

    memset(cap, 0, sizeof(*cap));

    const char drv[] = "avatar-uvc";
    for (size_t i = 0; i < sizeof(drv) && i < sizeof(cap->driver); i++)
        cap->driver[i] = drv[i];

    const char card[] = "UVC Camera";
    for (size_t i = 0; i < sizeof(card) && i < sizeof(cap->card); i++)
        cap->card[i] = card[i];

    cap->capabilities = AV_UVC_CAP_VIDEO_CAPTURE | AV_UVC_CAP_READWRITE;
    cap->device_caps = AV_UVC_CAP_VIDEO_CAPTURE | AV_UVC_CAP_READWRITE;
    cap->vid = g_uvc_state.enum_result.first_uvc_vid;
    cap->pid = g_uvc_state.enum_result.first_uvc_pid;
    cap->dev_addr = g_uvc_state.enum_result.first_uvc_addr;
    return 0;
}

static int uvc_video_get_format(struct avatar_uvc_format *fmt)
{
    if (!fmt)
        return -UVC_ERR_INVAL;
    if (!g_uvc_state.present)
        return -UVC_ERR_NODEV;

    memset(fmt, 0, sizeof(*fmt));
    fmt->pixelformat = AV_PIXFMT_MJPEG;
    return 0;
}

static int uvc_video_get_frame(struct avatar_uvc_frame *dst)
{
    if (!dst)
        return -UVC_ERR_INVAL;
    if (!g_uvc_state.present)
        return -UVC_ERR_NODEV;

    usb_uvc_frame_t frame;
    int rc = dwc2_usb_capture_first_uvc_frame(&g_uvc_state.enum_result, &frame);
    if (rc != 0)
        return -UVC_ERR_IO;

    uint8_t *user_buf = (uint8_t *)(uintptr_t)dst->buf;
    uint32_t copy_len = frame.length;
    if (user_buf && dst->buf_size > 0) {
        if (copy_len > dst->buf_size)
            copy_len = dst->buf_size;
        memcpy(user_buf, frame.data, copy_len);
    }

    dst->bytes_used = frame.length;
    dst->sequence = g_uvc_state.sequence++;
    dst->pixelformat = AV_PIXFMT_MJPEG;
    dst->transfers = frame.transfers;
    dst->data_packets = frame.data_packets;
    dst->fid = frame.fid;
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
        return g_uvc_state.present ? 0 : -UVC_ERR_NODEV;
    case AV_UVC_IOC_STREAMOFF:
        return g_uvc_state.present ? 0 : -UVC_ERR_NODEV;
    default:
        return -UVC_ERR_NOSYS;
    }
}
