#ifndef __USB_UVC_VIDEO_H__
#define __USB_UVC_VIDEO_H__

#include "types.h"
#include "usb/usb.h"

void uvc_video_clear(void);
int uvc_video_register_device(uint32_t dev_addr, const usb_device_info_t *dev);
int uvc_video_read_frame(void *buf, size_t len);
int uvc_video_ioctl(uint64_t req, void *argp);

#endif /* __USB_UVC_VIDEO_H__ */