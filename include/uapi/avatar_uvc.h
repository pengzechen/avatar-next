#ifndef __AVATAR_UVC_UAPI_H__
#define __AVATAR_UVC_UAPI_H__

typedef __UINT8_TYPE__  avatar_u8;
typedef __UINT16_TYPE__ avatar_u16;
typedef __UINT32_TYPE__ avatar_u32;
typedef __UINT64_TYPE__ avatar_u64;

#define AV_PIXFMT_MJPEG 0x47504a4dU /* 'MJPG' */

#define AV_UVC_CAP_VIDEO_CAPTURE 0x00000001U
#define AV_UVC_CAP_READWRITE     0x00000002U
#define AV_UVC_CAP_STREAMING     0x00000004U

#define AV_UVC_IOC_BASE          0x56000000U
#define AV_UVC_IOC_QUERYCAP      (AV_UVC_IOC_BASE + 0x01U)
#define AV_UVC_IOC_G_FMT         (AV_UVC_IOC_BASE + 0x02U)
#define AV_UVC_IOC_GET_FRAME     (AV_UVC_IOC_BASE + 0x03U)
#define AV_UVC_IOC_STREAMON      (AV_UVC_IOC_BASE + 0x04U)
#define AV_UVC_IOC_STREAMOFF     (AV_UVC_IOC_BASE + 0x05U)

struct avatar_uvc_cap {
    char driver[16];
    char card[32];
    char bus_info[32];
    avatar_u32 version;
    avatar_u32 capabilities;
    avatar_u32 device_caps;
    avatar_u16 vid;
    avatar_u16 pid;
    avatar_u8  dev_addr;
    avatar_u8  reserved[7];
};

struct avatar_uvc_format {
    avatar_u32 width;
    avatar_u32 height;
    avatar_u32 pixelformat;
    avatar_u32 frame_interval_100ns;
    avatar_u32 max_frame_size;
    avatar_u32 max_payload_size;
    avatar_u8  format_index;
    avatar_u8  frame_index;
    avatar_u8  interface_number;
    avatar_u8  endpoint_number;
    avatar_u8  alt_setting;
    avatar_u8  reserved[11];
};

struct avatar_uvc_frame {
    avatar_u64 buf;
    avatar_u32 buf_size;
    avatar_u32 bytes_used;
    avatar_u32 sequence;
    avatar_u32 pixelformat;
    avatar_u32 width;
    avatar_u32 height;
    avatar_u32 transfers;
    avatar_u32 data_packets;
    avatar_u8  fid;
    avatar_u8  reserved[7];
};

#endif /* __AVATAR_UVC_UAPI_H__ */