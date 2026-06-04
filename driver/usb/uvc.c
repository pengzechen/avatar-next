#include "usb/uvc.h"
#include "usb/usb_core_internal.h"
#include "klog.h"
#include "string.h"

#define USB_CLASS_VIDEO                    0x0e
#define USB_SUBCLASS_VIDEO_CONTROL         0x01
#define USB_SUBCLASS_VIDEO_STREAMING       0x02
#define USB_DESC_INTERFACE                 4
#define USB_DESC_ENDPOINT                  5
#define USB_DT_CS_INTERFACE                0x24
#define UVC_VS_FORMAT_UNCOMPRESSED         0x04
#define UVC_VS_FRAME_UNCOMPRESSED          0x05
#define UVC_VS_FORMAT_MJPEG                0x06
#define UVC_VS_FRAME_MJPEG                 0x07
#define USB_EP_ATTR_ISOCH                  0x01
#define USB_EP_ATTR_BULK                   0x02
#define UVC_VS_PROBE_CONTROL              0x01
#define UVC_VS_COMMIT_CONTROL             0x02
#define UVC_PROBE_COMMIT_LEN              34

#define UVC_MAX_ISOCH_ALTS                 8
#define UVC_RX_WORK_BYTES                  2048
#define UVC_CAPTURE_MAX_UFRAMES            80000u

static uint8_t g_uvc_rx_packet[UVC_RX_WORK_BYTES] __attribute__((aligned(256)));
static uint8_t g_uvc_frame_buf[USB_UVC_FRAME_BUFFER_SIZE] __attribute__((aligned(256)));
static uint8_t g_uvc_last_eof_fid = 0xff;

typedef struct {
    bool found;
    uint8_t format_index;
    uint8_t frame_index;
    uint16_t width;
    uint16_t height;
    uint32_t interval;
    bool is_mjpeg;
} uvc_frame_pick_t;

typedef struct {
    bool found;
    uint8_t alt;
    uint8_t ep_num;
    uint16_t mps_raw;
    uint8_t vs_if;
} uvc_ep_pick_t;

static inline uint16_t le16_load(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t le32_load(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static int uvc_frame_rank(const uvc_frame_pick_t *p)
{
    int area = (int)p->width * (int)p->height;
    if (p->width == 1280 && p->height == 720)
        return 1000000;
    if (p->width == 640 && p->height == 480)
        return 900000;
    if (p->width == 800 && p->height == 600)
        return 800000;
    if (p->width == 1024 && p->height == 768)
        return 750000;
    if (p->width == 320 && p->height == 240)
        return 700000;
    if (area <= 1280 * 720)
        return 600000 - (1280 * 720 - area);
    return 100000 - (area - 1280 * 720);
}

static void uvc_pick_frame(uvc_frame_pick_t *slot, const uvc_frame_pick_t *cand)
{
    if (!slot->found || uvc_frame_rank(cand) > uvc_frame_rank(slot))
        *slot = *cand;
}

static uint32_t uvc_interval_min(const uint8_t *desc, uint32_t off,
                                 uint32_t len, uint32_t dflt)
{
    uint8_t interval_type = desc[off + 25];
    uint32_t min_interval = dflt;

    if (interval_type == 0 && len >= 38) {
        uint32_t dw_min = le32_load(desc + off + 26);
        if (dw_min > 0)
            min_interval = dw_min;
    } else if (interval_type > 0) {
        uint32_t pos = off + 26;
        for (uint32_t n = 0; n < interval_type; n++) {
            if (pos + 4 > off + len)
                break;
            uint32_t value = le32_load(desc + pos);
            if (value > 0 && (min_interval == 0 || value < min_interval))
                min_interval = value;
            pos += 4;
        }
    }

    return min_interval ? min_interval : dflt;
}

static uint32_t uvc_mps_total(uint16_t mps_raw)
{
    return (uint32_t)(mps_raw & 0x7ff)
         * (uint32_t)(((mps_raw >> 11) & 0x3) + 1);
}

static uint32_t uvc_mps_low(uint16_t mps_raw)
{
    return mps_raw & 0x7ff;
}

static void uvc_record_isoch_alt(usb_device_info_t *dev, uint8_t alt,
                                 uint16_t mps_raw)
{
    if (dev->uvc_isoch_alts_count >= UVC_MAX_ISOCH_ALTS)
        return;
    uint8_t idx = dev->uvc_isoch_alts_count++;
    dev->uvc_isoch_alt_settings[idx] = alt;
    dev->uvc_isoch_mps_raw[idx] = mps_raw;
}

bool uvc_parse_config(const uint8_t *cfg, uint32_t total,
                      usb_device_info_t *dev)
{
    uint32_t off = 0;
    uint8_t cur_if_class = 0;
    uint8_t cur_if_sub = 0;
    uint8_t cur_if_num = 0;
    uint8_t cur_alt = 0;
    uint8_t cur_fmt_index = 0;
    uint8_t cur_fmt_subtype = 0;
    uint8_t vc_interface = 0;
    bool found_vc = false;
    bool found_vs = false;
    uvc_ep_pick_t best_bulk = {0};
    uvc_ep_pick_t best_isoch = {0};
    uvc_frame_pick_t best_mjpeg = {0};
    uvc_frame_pick_t best_uncomp = {0};

    dev->uvc_isoch_alts_count = 0;
    memset(dev->uvc_isoch_alt_settings, 0, sizeof(dev->uvc_isoch_alt_settings));
    memset(dev->uvc_isoch_mps_raw, 0, sizeof(dev->uvc_isoch_mps_raw));

    while (off + 2 <= total) {
        uint32_t len = cfg[off];
        uint8_t type = cfg[off + 1];
        if (len < 2 || off + len > total) {
            KLOG_WARN("[USB] descriptor walk stopped at off=%lu len=%lu total=%lu\n",
                      (unsigned long)off, (unsigned long)len, (unsigned long)total);
            break;
        }

        if (type == USB_DESC_INTERFACE && len >= sizeof(usb_interface_desc_t)) {
            cur_if_num = cfg[off + 2];
            cur_alt = cfg[off + 3];
            cur_if_class = cfg[off + 5];
            cur_if_sub = cfg[off + 6];
            if (cur_if_class == USB_CLASS_VIDEO) {
                KLOG_INFO("[USB] UVC iface: if=%u alt=%u eps=%u sub=%u proto=%u\n",
                          cur_if_num, cur_alt, cfg[off + 4], cur_if_sub, cfg[off + 7]);
                if (cur_if_sub == USB_SUBCLASS_VIDEO_CONTROL) {
                    found_vc = true;
                    vc_interface = cur_if_num;
                } else if (cur_if_sub == USB_SUBCLASS_VIDEO_STREAMING) {
                    found_vs = true;
                }
            }
        } else if (type == USB_DT_CS_INTERFACE
                   && cur_if_class == USB_CLASS_VIDEO
                   && cur_if_sub == USB_SUBCLASS_VIDEO_STREAMING
                   && len >= 3) {
            uint8_t subtype = cfg[off + 2];
            if ((subtype == UVC_VS_FORMAT_MJPEG || subtype == UVC_VS_FORMAT_UNCOMPRESSED)
                && len >= 4) {
                cur_fmt_subtype = subtype;
                cur_fmt_index = cfg[off + 3];
                KLOG_INFO("[USB] UVC format: if=%u alt=%u index=%u type=%s\n",
                          cur_if_num, cur_alt, cur_fmt_index,
                          subtype == UVC_VS_FORMAT_MJPEG ? "MJPEG" : "Uncompressed");
            } else if ((subtype == UVC_VS_FRAME_MJPEG || subtype == UVC_VS_FRAME_UNCOMPRESSED)
                       && len >= 26) {
                uvc_frame_pick_t pick;
                memset(&pick, 0, sizeof(pick));
                pick.found = true;
                pick.format_index = cur_fmt_index ? cur_fmt_index : 1;
                pick.frame_index = cfg[off + 3];
                pick.width = le16_load(cfg + off + 5);
                pick.height = le16_load(cfg + off + 7);
                pick.interval = uvc_interval_min(cfg, off, len, le32_load(cfg + off + 21));
                pick.is_mjpeg = (cur_fmt_subtype == UVC_VS_FORMAT_MJPEG
                              || subtype == UVC_VS_FRAME_MJPEG);
                uint32_t fps_x100 = pick.interval ? 100000000u / pick.interval : 0;
                KLOG_INFO("[USB] UVC frame: fmt=%u frame=%u %ux%u interval=%lu (%lu.%02lu fps) %s\n",
                          pick.format_index, pick.frame_index, pick.width, pick.height,
                          (unsigned long)pick.interval,
                          (unsigned long)(fps_x100 / 100), (unsigned long)(fps_x100 % 100),
                          pick.is_mjpeg ? "MJPEG" : "Uncompressed");
                if (pick.is_mjpeg)
                    uvc_pick_frame(&best_mjpeg, &pick);
                else
                    uvc_pick_frame(&best_uncomp, &pick);
            }
        } else if (type == USB_DESC_ENDPOINT
                   && cur_if_class == USB_CLASS_VIDEO
                   && cur_if_sub == USB_SUBCLASS_VIDEO_STREAMING
                   && len >= sizeof(usb_endpoint_desc_t)) {
            uint8_t ep_addr = cfg[off + 2];
            uint8_t attr = cfg[off + 3];
            uint16_t mps_raw = le16_load(cfg + off + 4);
            uint16_t mps = mps_raw & 0x7ff;
            uint16_t mult = ((mps_raw >> 11) & 0x3) + 1;
            uint8_t xfer = attr & 0x03;

            if (ep_addr & 0x80) {
                uint8_t ep_num = ep_addr & 0x0f;
                uint32_t total_bytes = (uint32_t)mps * (uint32_t)mult;
                KLOG_INFO("[USB] UVC endpoint: if=%u alt=%u ep=%u kind=%s mps=%u mult=%u total=%lu raw=0x%04x\n",
                          cur_if_num, cur_alt, ep_num,
                          xfer == USB_EP_ATTR_BULK ? "Bulk" :
                          xfer == USB_EP_ATTR_ISOCH ? "Isoch" : "Other",
                          mps, mult, (unsigned long)total_bytes, mps_raw);
                if (xfer == USB_EP_ATTR_BULK) {
                    if (!best_bulk.found || mps > (best_bulk.mps_raw & 0x7ff)) {
                        best_bulk.found = true;
                        best_bulk.alt = cur_alt;
                        best_bulk.ep_num = ep_num;
                        best_bulk.mps_raw = mps_raw;
                        best_bulk.vs_if = cur_if_num;
                    }
                } else if (xfer == USB_EP_ATTR_ISOCH) {
                    uint32_t old_mps = best_isoch.mps_raw & 0x7ff;
                    uint32_t old_mult = ((best_isoch.mps_raw >> 11) & 0x3) + 1;
                    uint32_t old_score = old_mult == 1 ? 10000000u + old_mps : old_mps * old_mult;
                    uint32_t new_score = mult == 1 ? 10000000u + mps : (uint32_t)mps * mult;
                    if (!best_isoch.found || new_score > old_score) {
                        best_isoch.found = true;
                        best_isoch.alt = cur_alt;
                        best_isoch.ep_num = ep_num;
                        best_isoch.mps_raw = mps_raw;
                        best_isoch.vs_if = cur_if_num;
                    }
                    uvc_record_isoch_alt(dev, cur_alt, mps_raw);
                }
            }
        }

        off += len;
    }

    if (!found_vc && !found_vs)
        return false;

    dev->is_uvc = true;
    dev->uvc_vc_interface = vc_interface;

    uvc_ep_pick_t ep = best_bulk.found ? best_bulk : best_isoch;
    if (ep.found) {
        uvc_frame_pick_t frame = best_mjpeg.found ? best_mjpeg : best_uncomp;
        dev->uvc_vs_interface = ep.vs_if;
        dev->uvc_alt_setting = ep.alt;
        dev->uvc_ep_num = ep.ep_num;
        dev->uvc_xfer_type = best_bulk.found ? USB_EP_ATTR_BULK : USB_EP_ATTR_ISOCH;
        dev->uvc_mps_raw = ep.mps_raw;
        if (frame.found) {
            dev->uvc_format_index = frame.format_index;
            dev->uvc_frame_index = frame.frame_index;
            dev->uvc_is_mjpeg = frame.is_mjpeg;
            dev->uvc_frame_width = frame.width;
            dev->uvc_frame_height = frame.height;
            dev->uvc_frame_interval = frame.interval;
        } else {
            dev->uvc_format_index = 1;
            dev->uvc_frame_index = 1;
            dev->uvc_frame_interval = 333333;
        }
        KLOG_INFO("[USB] UVC selected: vc_if=%u vs_if=%u alt=%u ep=%u %s mps_raw=0x%04x fmt=%u frame=%u %ux%u interval=%lu mjpeg=%u\n",
                  dev->uvc_vc_interface, dev->uvc_vs_interface,
                  dev->uvc_alt_setting, dev->uvc_ep_num,
                  dev->uvc_xfer_type == USB_EP_ATTR_BULK ? "Bulk" : "Isoch",
                  dev->uvc_mps_raw, dev->uvc_format_index, dev->uvc_frame_index,
                  dev->uvc_frame_width, dev->uvc_frame_height,
                  (unsigned long)dev->uvc_frame_interval, dev->uvc_is_mjpeg ? 1u : 0u);
    } else {
        KLOG_WARN("[USB] UVC interfaces found, but no VideoStreaming IN endpoint yet\n");
    }

    return true;
}

static void make_setup_uvc_set_cur_vs(uint8_t interface, uint8_t selector,
                                      uint16_t w_length, uint8_t out[8])
{
    uint16_t w_value = (uint16_t)selector << 8;
    out[0] = 0x21;
    out[1] = 0x01;
    out[2] = (uint8_t)(w_value & 0xff);
    out[3] = (uint8_t)(w_value >> 8);
    out[4] = interface;
    out[5] = 0;
    out[6] = (uint8_t)(w_length & 0xff);
    out[7] = (uint8_t)(w_length >> 8);
}

static void make_setup_uvc_get_cur_vs(uint8_t interface, uint8_t selector,
                                      uint16_t w_length, uint8_t out[8])
{
    uint16_t w_value = (uint16_t)selector << 8;
    out[0] = 0xa1;
    out[1] = 0x81;
    out[2] = (uint8_t)(w_value & 0xff);
    out[3] = (uint8_t)(w_value >> 8);
    out[4] = interface;
    out[5] = 0;
    out[6] = (uint8_t)(w_length & 0xff);
    out[7] = (uint8_t)(w_length >> 8);
}

static void make_setup_uvc_get_max_vs(uint8_t interface, uint8_t selector,
                                      uint16_t w_length, uint8_t out[8])
{
    uint16_t w_value = (uint16_t)selector << 8;
    out[0] = 0xa1;
    out[1] = 0x83;
    out[2] = (uint8_t)(w_value & 0xff);
    out[3] = (uint8_t)(w_value >> 8);
    out[4] = interface;
    out[5] = 0;
    out[6] = (uint8_t)(w_length & 0xff);
    out[7] = (uint8_t)(w_length >> 8);
}

static void build_uvc_probe_commit_payload(const usb_device_info_t *dev,
                                           uint8_t out[UVC_PROBE_COMMIT_LEN])
{
    memset(out, 0, UVC_PROBE_COMMIT_LEN);
    out[0] = 0x01;
    out[1] = 0x00;
    out[2] = dev->uvc_format_index ? dev->uvc_format_index : 1;
    out[3] = dev->uvc_frame_index ? dev->uvc_frame_index : 1;
    put_le32(out + 4, dev->uvc_frame_interval ? dev->uvc_frame_interval : 333333);

    uint32_t width = dev->uvc_frame_width ? dev->uvc_frame_width : 640;
    uint32_t height = dev->uvc_frame_height ? dev->uvc_frame_height : 480;
    if (width < 640)
        width = 640;
    if (height < 480)
        height = 480;
    uint32_t frame_size = dev->uvc_is_mjpeg ? width * height : width * height * 2;
    uint32_t payload_size = uvc_mps_total(dev->uvc_mps_raw);
    put_le32(out + 18, frame_size);
    put_le32(out + 22, payload_size);
}

static void dump_uvc_probe(const char *prefix, const uint8_t *p)
{
    KLOG_INFO("[USB] UVC %s bmHint=0x%04x fmt=%u frame=%u iv=%lu "
              "keyFrm=%u pFrm=%u compQ=%u compW=%u delay=%u "
              "dwMaxVideoFrameSize=%lu dwMaxPayloadTransferSize=%lu\n",
              prefix, le16_load(p + 0), p[2], p[3],
              (unsigned long)le32_load(p + 4), le16_load(p + 8),
              le16_load(p + 10), le16_load(p + 12), le16_load(p + 14),
              le16_load(p + 16), (unsigned long)le32_load(p + 18),
              (unsigned long)le32_load(p + 22));
}

static void uvc_reselect_isoch_alt_for_payload(usb_device_info_t *dev,
                                               uint32_t payload)
{
    if (dev->uvc_xfer_type != USB_EP_ATTR_ISOCH || payload == 0)
        return;

    bool have_fit = false;
    bool have_max = false;
    uint8_t fit_alt = dev->uvc_alt_setting;
    uint16_t fit_mps = dev->uvc_mps_raw;
    uint32_t fit_total = 0xffffffffu;
    uint8_t max_alt = dev->uvc_alt_setting;
    uint16_t max_mps = dev->uvc_mps_raw;
    uint32_t max_total = 0;

    for (uint8_t i = 0; i < dev->uvc_isoch_alts_count; i++) {
        uint16_t mps_raw = dev->uvc_isoch_mps_raw[i];
        uint32_t mult = ((mps_raw >> 11) & 0x3) + 1;
        if (mult > 1)
            continue;
        uint32_t total = mps_raw & 0x7ff;
        if (total >= payload && total < fit_total) {
            have_fit = true;
            fit_alt = dev->uvc_isoch_alt_settings[i];
            fit_mps = mps_raw;
            fit_total = total;
        }
        if (total > max_total) {
            have_max = true;
            max_alt = dev->uvc_isoch_alt_settings[i];
            max_mps = mps_raw;
            max_total = total;
        }
    }

    uint8_t new_alt = have_fit ? fit_alt : max_alt;
    uint16_t new_mps = have_fit ? fit_mps : max_mps;
    uint32_t new_total = have_fit ? fit_total : max_total;
    if (!(have_fit || have_max))
        return;

    if (new_alt != dev->uvc_alt_setting || new_mps != dev->uvc_mps_raw) {
        KLOG_INFO("[USB] UVC reselect Isoch alt %u raw=0x%04x (%lu B/uframe) -> alt %u raw=0x%04x (%lu B/uframe) for payload=%lu\n",
                  dev->uvc_alt_setting, dev->uvc_mps_raw,
                  (unsigned long)uvc_mps_total(dev->uvc_mps_raw),
                  new_alt, new_mps, (unsigned long)new_total,
                  (unsigned long)payload);
        dev->uvc_alt_setting = new_alt;
        dev->uvc_mps_raw = new_mps;
    }
}

int uvc_start_video_stream(uint32_t dev_addr, uint32_t ep0_mps,
                           usb_device_info_t *dev)
{
    uint8_t setup[8];
    uint8_t probe[UVC_PROBE_COMMIT_LEN];
    uint8_t probe_max[UVC_PROBE_COMMIT_LEN];

    g_uvc_last_eof_fid = 0xff;

    KLOG_INFO("[USB] UVC start: reset VS if=%u to alt=0...\n",
              dev->uvc_vs_interface);
    (void)usb_set_interface(dev_addr, dev->uvc_vs_interface, 0, ep0_mps);

    build_uvc_probe_commit_payload(dev, probe);
    dump_uvc_probe("PROBE.SET", probe);

    make_setup_uvc_set_cur_vs(dev->uvc_vs_interface, UVC_VS_PROBE_CONTROL,
                              UVC_PROBE_COMMIT_LEN, setup);
    int rc = usb_ep0_control_write(dev_addr, setup, ep0_mps, probe,
                                   UVC_PROBE_COMMIT_LEN);
    if (rc != 0) {
        KLOG_ERROR("[USB] UVC PROBE SET_CUR failed: %d\n", rc);
        return rc;
    }

    make_setup_uvc_get_max_vs(dev->uvc_vs_interface, UVC_VS_PROBE_CONTROL,
                              UVC_PROBE_COMMIT_LEN, setup);
    rc = usb_ep0_control_read(dev_addr, setup, ep0_mps, probe_max,
                              UVC_PROBE_COMMIT_LEN);
    if (rc == 0)
        dump_uvc_probe("PROBE.MAX", probe_max);
    else
        KLOG_WARN("[USB] UVC PROBE GET_MAX failed/non-fatal: %d\n", rc);

    make_setup_uvc_get_cur_vs(dev->uvc_vs_interface, UVC_VS_PROBE_CONTROL,
                              UVC_PROBE_COMMIT_LEN, setup);
    rc = usb_ep0_control_read(dev_addr, setup, ep0_mps, probe,
                              UVC_PROBE_COMMIT_LEN);
    if (rc != 0) {
        KLOG_ERROR("[USB] UVC PROBE GET_CUR failed: %d\n", rc);
        return rc;
    }
    dump_uvc_probe("PROBE.CUR", probe);

    uint32_t negotiated_payload = le32_load(probe + 22);
    uint32_t negotiated_frame = le32_load(probe + 18);
    uvc_reselect_isoch_alt_for_payload(dev, negotiated_payload);

    uint32_t alt_payload = uvc_mps_total(dev->uvc_mps_raw);
    if (alt_payload > 0 && negotiated_payload > alt_payload) {
        KLOG_INFO("[USB] UVC clamp payload %lu -> %lu for selected alt bandwidth\n",
                  (unsigned long)negotiated_payload, (unsigned long)alt_payload);
        negotiated_payload = alt_payload;
        put_le32(probe + 22, alt_payload);
    }

    make_setup_uvc_set_cur_vs(dev->uvc_vs_interface, UVC_VS_COMMIT_CONTROL,
                              UVC_PROBE_COMMIT_LEN, setup);
    rc = usb_ep0_control_write(dev_addr, setup, ep0_mps, probe,
                               UVC_PROBE_COMMIT_LEN);
    if (rc != 0) {
        KLOG_ERROR("[USB] UVC COMMIT SET_CUR failed: %d\n", rc);
        return rc;
    }

    KLOG_INFO("[USB] UVC SET_INTERFACE if=%u alt=%u...\n",
              dev->uvc_vs_interface, dev->uvc_alt_setting);
    rc = usb_set_interface(dev_addr, dev->uvc_vs_interface,
                           dev->uvc_alt_setting, ep0_mps);
    if (rc != 0) {
        KLOG_ERROR("[USB] UVC SET_INTERFACE failed: %d\n", rc);
        return rc;
    }

    dev->uvc_negotiated_payload_size = negotiated_payload;
    dev->uvc_negotiated_frame_size = negotiated_frame;
    KLOG_INFO("[USB] UVC streaming armed: if=%u alt=%u ep=%u payload=%lu frame_size=%lu\n",
              dev->uvc_vs_interface, dev->uvc_alt_setting, dev->uvc_ep_num,
              (unsigned long)negotiated_payload, (unsigned long)negotiated_frame);
    return 0;
}

typedef struct {
    bool capturing;
    bool have_last_fid;
    uint8_t last_fid;
    uint8_t frame_fid;
    bool saw_data;
} uvc_capture_state_t;

static bool uvc_frame_has_eoi(uint32_t len)
{
    return len >= 2
        && g_uvc_frame_buf[len - 2] == 0xff
        && g_uvc_frame_buf[len - 1] == 0xd9;
}

static void uvc_state_start_capture(uvc_capture_state_t *state, uint8_t fid)
{
    state->capturing = true;
    state->frame_fid = fid;
    state->saw_data = false;
}

static int uvc_process_payload_packet(const uint8_t *pkt, uint32_t len,
                                      uvc_capture_state_t *state,
                                      uint32_t *jpeg_len, uint8_t *done_fid,
                                      bool *out_done)
{
    *out_done = false;
    if (len < 2)
        return 0;

    uint32_t header_len = pkt[0];
    if (header_len < 2 || header_len > len)
        return 0;

    const uint8_t *payload = pkt + header_len;
    uint32_t payload_len = len - header_len;
    uint8_t info = pkt[1];
    uint8_t fid = info & 0x01;
    bool eof = (info & 0x02) != 0;

    if (info & 0x40) {
        if (payload_len == 0) {
            KLOG_DEBUG("[USB] UVC header-only ERR packet ignored: info=0x%02x len=%lu\n",
                       info, (unsigned long)len);
        } else {
            KLOG_WARN("[USB] UVC packet ERR bit set: info=0x%02x len=%lu payload=%lu\n",
                      info, (unsigned long)len, (unsigned long)payload_len);
        }
        *jpeg_len = 0;
        state->capturing = false;
        state->saw_data = false;
        state->have_last_fid = true;
        state->last_fid = fid;
        return 0;
    }

    if (!state->capturing) {
        if (!state->have_last_fid) {
            state->have_last_fid = true;
            state->last_fid = fid;
            return 0;
        }
        if (state->last_fid == fid)
            return 0;
        uvc_state_start_capture(state, fid);
    }

    if (fid != state->frame_fid) {
        if (state->saw_data && uvc_frame_has_eoi(*jpeg_len)) {
            *done_fid = state->frame_fid;
            *out_done = true;
            return 0;
        }
        *jpeg_len = 0;
        uvc_state_start_capture(state, fid);
    }

    if (payload_len > 0) {
        if (!state->saw_data) {
            if (payload_len < 2 || payload[0] != 0xff || payload[1] != 0xd8)
                return 0;
        }
        if (*jpeg_len + payload_len > USB_UVC_FRAME_BUFFER_SIZE) {
            KLOG_ERROR("[USB] UVC frame buffer overflow len=%lu add=%lu cap=%lu\n",
                       (unsigned long)*jpeg_len, (unsigned long)payload_len,
                       (unsigned long)USB_UVC_FRAME_BUFFER_SIZE);
            return -1;
        }
        memcpy(g_uvc_frame_buf + *jpeg_len, payload, payload_len);
        *jpeg_len += payload_len;
        state->saw_data = true;
    }

    if (eof) {
        if (state->saw_data && uvc_frame_has_eoi(*jpeg_len)) {
            *done_fid = state->frame_fid;
            *out_done = true;
            return 0;
        }
        *jpeg_len = 0;
        state->capturing = false;
        state->have_last_fid = true;
        state->last_fid = fid;
    }

    return 0;
}

int uvc_capture_one_frame(uint32_t dev_addr, const usb_device_info_t *dev,
                          usb_uvc_frame_t *frame)
{
    if (dev == NULL || frame == NULL || !dev->is_uvc)
        return -1;
    memset(frame, 0, sizeof(*frame));

    if (dev->uvc_xfer_type != USB_EP_ATTR_ISOCH) {
        KLOG_ERROR("[USB] UVC capture: only Isoch MJPEG is implemented now (xfer=%u)\n",
                   dev->uvc_xfer_type);
        return -1;
    }
    if (!dev->uvc_is_mjpeg) {
        KLOG_ERROR("[USB] UVC capture: only MJPEG assembly is implemented now\n");
        return -1;
    }

    uint32_t mps_low = uvc_mps_low(dev->uvc_mps_raw);
    uint32_t mult = ((dev->uvc_mps_raw >> 11) & 0x3u) + 1u;
    uint32_t total = uvc_mps_total(dev->uvc_mps_raw);
    if (mps_low == 0 || total == 0 || total > sizeof(g_uvc_rx_packet))
        return -1;

    uvc_capture_state_t state;
    memset(&state, 0, sizeof(state));
    if (g_uvc_last_eof_fid <= 1) {
        state.have_last_fid = true;
        state.last_fid = g_uvc_last_eof_fid;
    }

    uint32_t jpeg_len = 0;
    uint32_t transfers = 0;
    uint32_t data_packets = 0;
    uint8_t done_fid = 0xff;

    KLOG_INFO("[USB] UVC capture: dev=%lu ep=%u mps_raw=0x%04x total=%lu mult=%lu cap=%lu\n",
              (unsigned long)dev_addr, dev->uvc_ep_num, dev->uvc_mps_raw,
              (unsigned long)total, (unsigned long)mult,
              (unsigned long)USB_UVC_FRAME_BUFFER_SIZE);

    for (uint32_t i = 0; i < UVC_CAPTURE_MAX_UFRAMES; i++) {
        uint32_t actual = 0;
        int rc = usb_isoch_in_packet(dev_addr, dev->uvc_ep_num, dev->uvc_mps_raw,
                                     g_uvc_rx_packet, sizeof(g_uvc_rx_packet),
                                     &actual);
        transfers++;
        if (rc != 0)
            return rc;
        if (actual == 0)
            continue;

        data_packets++;
        bool done = false;
        if (mult == 1) {
            rc = uvc_process_payload_packet(g_uvc_rx_packet, actual, &state,
                                            &jpeg_len, &done_fid, &done);
        } else {
            uint32_t off = 0;
            while (off < actual) {
                uint32_t pkt_len = actual - off;
                if (pkt_len > mps_low)
                    pkt_len = mps_low;
                rc = uvc_process_payload_packet(g_uvc_rx_packet + off, pkt_len,
                                                &state, &jpeg_len, &done_fid,
                                                &done);
                if (rc != 0 || done)
                    break;
                off += pkt_len;
            }
        }
        if (rc != 0)
            return rc;

        if (done) {
            g_uvc_last_eof_fid = done_fid;
            frame->data = g_uvc_frame_buf;
            frame->length = jpeg_len;
            frame->transfers = transfers;
            frame->data_packets = data_packets;
            frame->fid = done_fid;
            frame->is_mjpeg = true;
            KLOG_INFO("[USB] UVC frame captured: len=%lu transfers=%lu data_packets=%lu fid=%u soi=%02x%02x eoi=%02x%02x\n",
                      (unsigned long)jpeg_len, (unsigned long)transfers,
                      (unsigned long)data_packets, done_fid,
                      jpeg_len >= 2 ? g_uvc_frame_buf[0] : 0,
                      jpeg_len >= 2 ? g_uvc_frame_buf[1] : 0,
                      jpeg_len >= 2 ? g_uvc_frame_buf[jpeg_len - 2] : 0,
                      jpeg_len >= 2 ? g_uvc_frame_buf[jpeg_len - 1] : 0);
            return 0;
        }
    }

    KLOG_WARN("[USB] UVC capture timeout: transfers=%lu data_packets=%lu assembled=%lu\n",
              (unsigned long)transfers, (unsigned long)data_packets,
              (unsigned long)jpeg_len);
    return -1;
}
