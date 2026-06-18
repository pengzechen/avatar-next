#ifndef __USB_UVC_VIDEO_H__
#define __USB_UVC_VIDEO_H__

#include "types.h"
#include "usb_api.h"

void uvc_video_clear(void);
void uvc_video_set_enum_result(const usb_enumerate_result_t *result);
int uvc_video_read_frame(void *buf, size_t len);
int uvc_video_ioctl(uint64_t req, void *argp);

#endif /* __USB_UVC_VIDEO_H__ */
