#ifndef __USB_UVC_H__
#define __USB_UVC_H__

#include "types.h"
#include "usb/usb.h"

bool uvc_parse_config(const uint8_t *cfg, uint32_t total,
                      usb_device_info_t *dev);
int uvc_start_video_stream(uint32_t dev_addr, uint32_t ep0_mps,
                           usb_device_info_t *dev);
int uvc_capture_one_frame(uint32_t dev_addr, const usb_device_info_t *dev,
                          usb_uvc_frame_t *frame);

#endif /* __USB_UVC_H__ */
