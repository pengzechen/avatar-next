#ifndef __USB_CORE_INTERNAL_H__
#define __USB_CORE_INTERNAL_H__

#include "types.h"

int usb_ep0_control_write_no_data(uint32_t dev, const uint8_t setup[8],
                                  uint32_t ep0_mps);
int usb_ep0_control_read(uint32_t dev, const uint8_t setup[8],
                         uint32_t ep0_mps, uint8_t *out, uint32_t out_len);
int usb_ep0_control_write(uint32_t dev, const uint8_t setup[8],
                          uint32_t ep0_mps, const uint8_t *data,
                          uint32_t data_len);
int usb_set_interface(uint32_t dev, uint8_t interface, uint8_t alt,
                      uint32_t ep0_mps);
int usb_isoch_in_packet(uint32_t dev, uint8_t ep, uint16_t mps_raw,
                        uint8_t *buf, uint32_t cap, uint32_t *out_actual);
int usb_isoch_channel_setup(uint32_t dev, uint8_t ep, uint16_t mps_raw);
int usb_isoch_in_packet_fast(uint32_t dev, uint8_t ep, uint16_t mps_raw,
                             uint8_t *buf, uint32_t cap, uint32_t *out_actual);

#endif /* __USB_CORE_INTERNAL_H__ */
