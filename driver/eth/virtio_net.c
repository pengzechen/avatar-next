#include "eth/virtio_net.h"

#include "barrier.h"
#include "klog.h"
#include "mmio.h"
#include "mm_vm.h"
#include "net/netdev.h"
#include "pmm.h"
#include "string.h"
#include "task/task.h"
#include "timer/timer.h"

#define VIRTIO_MMIO_MAGIC_VALUE             0x000U
#define VIRTIO_MMIO_VERSION                 0x004U
#define VIRTIO_MMIO_DEVICE_ID               0x008U
#define VIRTIO_MMIO_VENDOR_ID               0x00cU
#define VIRTIO_MMIO_DEVICE_FEATURES         0x010U
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL     0x014U
#define VIRTIO_MMIO_DRIVER_FEATURES         0x020U
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL     0x024U
#define VIRTIO_MMIO_GUEST_PAGE_SIZE         0x028U
#define VIRTIO_MMIO_QUEUE_SEL               0x030U
#define VIRTIO_MMIO_QUEUE_NUM_MAX           0x034U
#define VIRTIO_MMIO_QUEUE_NUM               0x038U
#define VIRTIO_MMIO_QUEUE_ALIGN             0x03cU
#define VIRTIO_MMIO_QUEUE_PFN               0x040U
#define VIRTIO_MMIO_QUEUE_READY             0x044U
#define VIRTIO_MMIO_QUEUE_NOTIFY            0x050U
#define VIRTIO_MMIO_INTERRUPT_STATUS        0x060U
#define VIRTIO_MMIO_INTERRUPT_ACK           0x064U
#define VIRTIO_MMIO_STATUS                  0x070U
#define VIRTIO_MMIO_QUEUE_DESC_LOW          0x080U
#define VIRTIO_MMIO_QUEUE_DESC_HIGH         0x084U
#define VIRTIO_MMIO_QUEUE_DRIVER_LOW        0x090U
#define VIRTIO_MMIO_QUEUE_DRIVER_HIGH       0x094U
#define VIRTIO_MMIO_QUEUE_DEVICE_LOW        0x0a0U
#define VIRTIO_MMIO_QUEUE_DEVICE_HIGH       0x0a4U
#define VIRTIO_MMIO_CONFIG_GENERATION       0x0fcU
#define VIRTIO_MMIO_CONFIG                  0x100U

#define VIRTIO_MMIO_MAGIC                   0x74726976U
#define VIRTIO_DEVICE_ID_NET                1U

#define VIRTIO_STATUS_ACKNOWLEDGE           0x01U
#define VIRTIO_STATUS_DRIVER                0x02U
#define VIRTIO_STATUS_DRIVER_OK             0x04U
#define VIRTIO_STATUS_FEATURES_OK           0x08U
#define VIRTIO_STATUS_FAILED                0x80U

#define VIRTIO_F_ANY_LAYOUT                 27U
#define VIRTIO_RING_F_INDIRECT_DESC         28U
#define VIRTIO_RING_F_EVENT_IDX             29U
#define VIRTIO_F_VERSION_1                  32U

#define VIRTIO_NET_F_MAC                    5U
#define VIRTIO_NET_F_STATUS                 16U
#define VIRTIO_NET_F_MRG_RXBUF              15U

#define VRING_DESC_F_NEXT                   1U
#define VRING_DESC_F_WRITE                  2U

#define VIRTIO_NET_Q_RX                     0U
#define VIRTIO_NET_Q_TX                     1U
#define VIRTIO_NET_QUEUE_SIZE               16U
#define VIRTIO_NET_RX_BUFS                  VIRTIO_NET_QUEUE_SIZE
#define VIRTIO_NET_TX_BUFS                  8U

#define VIRTIO_NET_HDR_LEN                  10U
#define ETH_FRAME_MAX                       1514U
#define ETH_FRAME_MIN                       60U
#define ETH_BUF_SIZE                        2048U

#ifndef DEVICE_ETH_BASE_RAW
#define DEVICE_ETH_BASE_RAW                 0U
#endif

#ifndef DEVICE_MMIO_NEEDS_VMA
#define DEVICE_MMIO_NEEDS_VMA               0
#endif

#if DEVICE_MMIO_NEEDS_VMA
#include "mm_vm.h"
#define VIRTIO_NET_PLATFORM_BASE ((uintptr_t)DEVICE_ETH_BASE_RAW + KERNEL_VMA)
#else
#define VIRTIO_NET_PLATFORM_BASE ((uintptr_t)DEVICE_ETH_BASE_RAW)
#endif

typedef struct PACKED virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} virtq_desc_t;

typedef struct PACKED virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIRTIO_NET_QUEUE_SIZE];
    uint16_t used_event;
} virtq_avail_t;

typedef struct PACKED virtq_used_elem {
    uint32_t id;
    uint32_t len;
} virtq_used_elem_t;

typedef struct PACKED virtq_used {
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[VIRTIO_NET_QUEUE_SIZE];
    uint16_t avail_event;
} virtq_used_t;

typedef struct ALIGNED(4096) virtio_queue {
    virtq_desc_t desc[VIRTIO_NET_QUEUE_SIZE];
    virtq_avail_t avail;
    uint8_t pad[4096 - sizeof(virtq_desc_t) * VIRTIO_NET_QUEUE_SIZE
                - sizeof(virtq_avail_t)];
    virtq_used_t used;
    uint16_t free_head;
    uint16_t last_used_idx;
    uint16_t num;
} virtio_queue_t;

typedef struct ALIGNED(16) virtio_net_buf {
    uint8_t hdr[VIRTIO_NET_HDR_LEN];
    uint8_t frame[ETH_BUF_SIZE];
} virtio_net_buf_t;

struct virtio_net_nic {
    uintptr_t base;
    uint32_t version;
    uint32_t ready;
    uint64_t negotiated_features;
    uint8_t mac[6];
    virtio_queue_t rxq;
    virtio_queue_t txq;
    virtio_net_buf_t rx_bufs[VIRTIO_NET_RX_BUFS];
    virtio_net_buf_t tx_bufs[VIRTIO_NET_TX_BUFS];
    uint8_t tx_busy[VIRTIO_NET_QUEUE_SIZE];
    uint16_t tx_head_to_buf[VIRTIO_NET_QUEUE_SIZE];
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_drops;
    uint64_t tx_busy_count;
};

static VirtioNetNic_t *g_eth0;
static uint64_t g_virtio_net0_pa;

static int virtio_netdev_send(void *ctx, const uint8_t *frame, size_t len)
{
    return eth_send((VirtioNetNic_t *)ctx, frame, len);
}

static int virtio_netdev_recv(void *ctx, uint8_t *frame, size_t maxlen)
{
    return eth_recv((VirtioNetNic_t *)ctx, frame, maxlen);
}

static inline uint32_t vn_read(struct virtio_net_nic *nic, uint32_t off)
{
    return read32((void *)(nic->base + off));
}

static inline void vn_write(struct virtio_net_nic *nic, uint32_t off, uint32_t val)
{
    write32(val, (void *)(nic->base + off));
}

static uint64_t virtio_read_features(struct virtio_net_nic *nic)
{
    if (nic->version == 1U) {
        vn_write(nic, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0U);
        return vn_read(nic, VIRTIO_MMIO_DEVICE_FEATURES);
    }

    vn_write(nic, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0U);
    uint64_t lo = vn_read(nic, VIRTIO_MMIO_DEVICE_FEATURES);
    vn_write(nic, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1U);
    uint64_t hi = vn_read(nic, VIRTIO_MMIO_DEVICE_FEATURES);
    return lo | (hi << 32U);
}

static void virtio_write_features(struct virtio_net_nic *nic, uint64_t features)
{
    vn_write(nic, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0U);
    vn_write(nic, VIRTIO_MMIO_DRIVER_FEATURES, (uint32_t)features);
    if (nic->version == 1U)
        return;

    vn_write(nic, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1U);
    vn_write(nic, VIRTIO_MMIO_DRIVER_FEATURES, (uint32_t)(features >> 32U));
}

static uintptr_t virtio_dma_addr(const void *ptr)
{
    uintptr_t va = (uintptr_t)ptr;
#if DEVICE_MMIO_NEEDS_VMA
    if (va >= KERNEL_VMA)
        return va - KERNEL_VMA;
#endif
    return va;
}

static void virtio_status_or(struct virtio_net_nic *nic, uint32_t bits)
{
    uint32_t s = vn_read(nic, VIRTIO_MMIO_STATUS);
    vn_write(nic, VIRTIO_MMIO_STATUS, s | bits);
}

static int virtio_reset(struct virtio_net_nic *nic)
{
    vn_write(nic, VIRTIO_MMIO_STATUS, 0U);
    for (uint32_t i = 0; i < 100000U; i++) {
        if (vn_read(nic, VIRTIO_MMIO_STATUS) == 0U)
            return 0;
    }
    KLOG_ERROR("[virtio-net] reset timeout status=0x%x\n",
               vn_read(nic, VIRTIO_MMIO_STATUS));
    return -1;
}

static void virtq_init_free(virtio_queue_t *q)
{
    q->free_head = 0;
    q->last_used_idx = 0;
    q->num = VIRTIO_NET_QUEUE_SIZE;
    q->avail.flags = 0;
    q->avail.idx = 0;
    q->used.flags = 0;
    q->used.idx = 0;

    for (uint16_t i = 0; i < VIRTIO_NET_QUEUE_SIZE; i++) {
        q->desc[i].addr = 0;
        q->desc[i].len = 0;
        q->desc[i].flags = (i + 1U < VIRTIO_NET_QUEUE_SIZE) ? VRING_DESC_F_NEXT : 0U;
        q->desc[i].next = i + 1U;
    }
}

static int virtq_alloc_desc(virtio_queue_t *q)
{
    if (q->free_head >= q->num)
        return -1;

    uint16_t id = q->free_head;
    if (q->desc[id].flags & VRING_DESC_F_NEXT)
        q->free_head = q->desc[id].next;
    else
        q->free_head = q->num;

    q->desc[id].flags = 0;
    q->desc[id].next = 0;
    return id;
}

static void virtq_free_desc(virtio_queue_t *q, uint16_t id)
{
    q->desc[id].flags = (q->free_head < q->num) ? VRING_DESC_F_NEXT : 0U;
    q->desc[id].next = q->free_head;
    q->desc[id].addr = 0;
    q->desc[id].len = 0;
    q->free_head = id;
}

static void virtq_free_chain(virtio_queue_t *q, uint16_t id)
{
    for (;;) {
        uint16_t flags = q->desc[id].flags;
        uint16_t next = q->desc[id].next;
        virtq_free_desc(q, id);
        if (!(flags & VRING_DESC_F_NEXT))
            break;
        id = next;
    }
}

static void virtq_push_avail(struct virtio_net_nic *nic, virtio_queue_t *q,
                             uint16_t qsel, uint16_t id)
{
    uint16_t slot = q->avail.idx % q->num;
    q->avail.ring[slot] = id;
    wmb();
    q->avail.idx++;
    wmb();
    vn_write(nic, VIRTIO_MMIO_QUEUE_NOTIFY, qsel);
}

static int virtio_setup_queue(struct virtio_net_nic *nic, virtio_queue_t *q,
                              uint32_t qsel, const char *name)
{
    vn_write(nic, VIRTIO_MMIO_QUEUE_SEL, qsel);
    uint32_t max = vn_read(nic, VIRTIO_MMIO_QUEUE_NUM_MAX);
    uint32_t ready = vn_read(nic, VIRTIO_MMIO_QUEUE_READY);

    KLOG_DEBUG("[virtio-net] queue %s sel=%u max=%u ready=%u desc=%p/0x%lx avail=%p/0x%lx used=%p/0x%lx\n",
               name, qsel, max, ready,
               q->desc, (unsigned long)virtio_dma_addr(q->desc),
               &q->avail, (unsigned long)virtio_dma_addr(&q->avail),
               &q->used, (unsigned long)virtio_dma_addr(&q->used));

    if (max < VIRTIO_NET_QUEUE_SIZE || max == 0U) {
        KLOG_ERROR("[virtio-net] queue %s unsupported max=%u need=%u\n",
                   name, max, VIRTIO_NET_QUEUE_SIZE);
        return -1;
    }

    virtq_init_free(q);
    vn_write(nic, VIRTIO_MMIO_QUEUE_NUM, VIRTIO_NET_QUEUE_SIZE);

    if (nic->version == 1U) {
        uintptr_t paddr = virtio_dma_addr(q);
        vn_write(nic, VIRTIO_MMIO_QUEUE_ALIGN, 4096U);
        vn_write(nic, VIRTIO_MMIO_QUEUE_PFN, (uint32_t)(paddr >> 12U));
        KLOG_DEBUG("[virtio-net] queue %s legacy paddr=0x%lx pfn=0x%x align=%u\n",
                   name, (unsigned long)paddr,
                   vn_read(nic, VIRTIO_MMIO_QUEUE_PFN),
                   vn_read(nic, VIRTIO_MMIO_QUEUE_ALIGN));
    } else {
        uintptr_t desc = virtio_dma_addr(q->desc);
        uintptr_t avail = virtio_dma_addr(&q->avail);
        uintptr_t used = virtio_dma_addr(&q->used);
        vn_write(nic, VIRTIO_MMIO_QUEUE_DESC_LOW, (uint32_t)desc);
        vn_write(nic, VIRTIO_MMIO_QUEUE_DESC_HIGH, (uint32_t)(desc >> 32U));
        vn_write(nic, VIRTIO_MMIO_QUEUE_DRIVER_LOW, (uint32_t)avail);
        vn_write(nic, VIRTIO_MMIO_QUEUE_DRIVER_HIGH, (uint32_t)(avail >> 32U));
        vn_write(nic, VIRTIO_MMIO_QUEUE_DEVICE_LOW, (uint32_t)used);
        vn_write(nic, VIRTIO_MMIO_QUEUE_DEVICE_HIGH, (uint32_t)(used >> 32U));
        wmb();
        vn_write(nic, VIRTIO_MMIO_QUEUE_READY, 1U);
    }
    wmb();

    KLOG_DEBUG("[virtio-net] queue %s ready num=%u qready=%u\n",
               name, VIRTIO_NET_QUEUE_SIZE, vn_read(nic, VIRTIO_MMIO_QUEUE_READY));
    return 0;
}

static int virtio_net_refill_rx(struct virtio_net_nic *nic)
{
    int added = 0;
    for (;;) {
        int id = virtq_alloc_desc(&nic->rxq);
        if (id < 0)
            break;

        virtio_net_buf_t *buf = &nic->rx_bufs[id];
        memset(buf, 0, sizeof(*buf));
        nic->rxq.desc[id].addr = (uint64_t)virtio_dma_addr(buf);
        nic->rxq.desc[id].len = sizeof(*buf);
        nic->rxq.desc[id].flags = VRING_DESC_F_WRITE;
        nic->rxq.desc[id].next = 0;
        virtq_push_avail(nic, &nic->rxq, VIRTIO_NET_Q_RX, (uint16_t)id);
        added++;
    }

    if (added) {
        KLOG_DEBUG("[virtio-net] rx refill added=%d avail_idx=%u free_head=%u\n",
                   added, nic->rxq.avail.idx, nic->rxq.free_head);
    }
    return added;
}

static void virtio_net_reclaim_tx(struct virtio_net_nic *nic)
{
    rmb();
    if (nic->txq.last_used_idx == nic->txq.used.idx) {
        KLOG_DEBUG("[virtio-net] tx reclaim none last_used=%u used_idx=%u avail_idx=%u isr=0x%x status=0x%x\n",
                   nic->txq.last_used_idx, nic->txq.used.idx,
                   nic->txq.avail.idx,
                   vn_read(nic, VIRTIO_MMIO_INTERRUPT_STATUS),
                   vn_read(nic, VIRTIO_MMIO_STATUS));
        return;
    }

    while (nic->txq.last_used_idx != nic->txq.used.idx) {
        virtq_used_elem_t *e =
            &nic->txq.used.ring[nic->txq.last_used_idx % nic->txq.num];
        uint16_t id = (uint16_t)e->id;
        if (id < VIRTIO_NET_QUEUE_SIZE) {
            uint16_t buf_id = nic->tx_head_to_buf[id];
            nic->tx_busy[id] = 0;
            virtq_free_chain(&nic->txq, id);
            KLOG_DEBUG("[virtio-net] tx complete id=%u buf=%u used_len=%u last_used=%u\n",
                       id, buf_id, e->len, nic->txq.last_used_idx);
        } else {
            KLOG_WARN("[virtio-net] tx used invalid id=%u\n", id);
        }
        nic->txq.last_used_idx++;
    }
}

VirtioNetNic_t *eth_init(uint64_t base)
{
    uint32_t nic_pages = (uint32_t)DIV_ROUND_UP(sizeof(struct virtio_net_nic), 4096U);
    uint64_t nic_pa = pmm_alloc_pages(g_pmm, nic_pages);
    if (nic_pa == 0U) {
        KLOG_ERROR("[virtio-net] pmm_alloc_pages(%u) for nic failed\n", nic_pages);
        return NULL;
    }

    struct virtio_net_nic *nic = (struct virtio_net_nic *)phys_to_virt(nic_pa);
    memset(nic, 0, sizeof(*nic));
    g_virtio_net0_pa = nic_pa;
    nic->base = (uintptr_t)base;

    KLOG_INFO("[virtio-net] init begin base=0x%llx nic_va=%p nic_pa=0x%llx pages=%u size=%zu\n",
              (unsigned long long)base, nic, (unsigned long long)nic_pa,
              nic_pages, sizeof(*nic));

    uint32_t magic = vn_read(nic, VIRTIO_MMIO_MAGIC_VALUE);
    uint32_t version = vn_read(nic, VIRTIO_MMIO_VERSION);
    uint32_t device = vn_read(nic, VIRTIO_MMIO_DEVICE_ID);
    uint32_t vendor = vn_read(nic, VIRTIO_MMIO_VENDOR_ID);
    uint32_t status = vn_read(nic, VIRTIO_MMIO_STATUS);
    uint32_t gen = vn_read(nic, VIRTIO_MMIO_CONFIG_GENERATION);

    KLOG_DEBUG("[virtio-net] mmio magic=0x%x version=%u device=%u vendor=0x%x status=0x%x cfg_gen=%u\n",
               magic, version, device, vendor, status, gen);

    nic->version = version;
    if (magic != VIRTIO_MMIO_MAGIC || (version != 1U && version != 2U)) {
        KLOG_ERROR("[virtio-net] unsupported transport magic=0x%x version=%u device=%u\n",
                   magic, version, device);
        return NULL;
    }

    if (device == 0U) {
        KLOG_WARN("[virtio-net] no device attached at transport base=0x%llx; use run-net or pass QEMU_NET_FLAGS\n",
                  (unsigned long long)base);
        return NULL;
    }

    if (device != VIRTIO_DEVICE_ID_NET) {
        KLOG_ERROR("[virtio-net] unsupported device id=%u at transport base=0x%llx\n",
                   device, (unsigned long long)base);
        return NULL;
    }

    if (virtio_reset(nic) != 0)
        return NULL;
    virtio_status_or(nic, VIRTIO_STATUS_ACKNOWLEDGE);
    virtio_status_or(nic, VIRTIO_STATUS_DRIVER);

    uint64_t dev_features = virtio_read_features(nic);
    uint64_t want = 0;
    if (version == 2U)
        want |= BIT64(VIRTIO_F_VERSION_1);
    if (version == 1U && (dev_features & BIT64(VIRTIO_F_ANY_LAYOUT)))
        want |= BIT64(VIRTIO_F_ANY_LAYOUT);
    if (dev_features & BIT64(VIRTIO_NET_F_MAC))
        want |= BIT64(VIRTIO_NET_F_MAC);

    KLOG_DEBUG("[virtio-net] device_features=0x%llx want=0x%llx unsupported_masked=0x%llx\n",
               (unsigned long long)dev_features,
               (unsigned long long)want,
               (unsigned long long)(dev_features & ~(want | BIT64(VIRTIO_F_ANY_LAYOUT)
                                                     | BIT64(VIRTIO_RING_F_INDIRECT_DESC)
                                                     | BIT64(VIRTIO_RING_F_EVENT_IDX)
                                                     | BIT64(VIRTIO_NET_F_MRG_RXBUF))));

    if (version == 2U && !(dev_features & BIT64(VIRTIO_F_VERSION_1))) {
        KLOG_ERROR("[virtio-net] device lacks VIRTIO_F_VERSION_1\n");
        vn_write(nic, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return NULL;
    }

    nic->negotiated_features = want;
    virtio_write_features(nic, want);
    if (version == 1U) {
        vn_write(nic, VIRTIO_MMIO_GUEST_PAGE_SIZE, 4096U);
        KLOG_DEBUG("[virtio-net] legacy guest_page_size=%u\n",
                   vn_read(nic, VIRTIO_MMIO_GUEST_PAGE_SIZE));
    }
    if (version == 2U) {
        virtio_status_or(nic, VIRTIO_STATUS_FEATURES_OK);
        status = vn_read(nic, VIRTIO_MMIO_STATUS);
        KLOG_DEBUG("[virtio-net] status after FEATURES_OK=0x%x\n", status);
        if (!(status & VIRTIO_STATUS_FEATURES_OK)) {
            KLOG_ERROR("[virtio-net] FEATURES_OK rejected status=0x%x\n", status);
            vn_write(nic, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
            return NULL;
        }
    } else {
        KLOG_DEBUG("[virtio-net] legacy device: FEATURES_OK handshake skipped\n");
    }

    if (want & BIT64(VIRTIO_NET_F_MAC)) {
        for (uint32_t i = 0; i < 6U; i++)
            nic->mac[i] = read8((void *)(nic->base + VIRTIO_MMIO_CONFIG + i));
    } else {
        nic->mac[0] = 0x52;
        nic->mac[1] = 0x54;
        nic->mac[2] = 0x00;
        nic->mac[3] = 0x12;
        nic->mac[4] = 0x34;
        nic->mac[5] = 0x56;
    }

    KLOG_INFO("[virtio-net] MAC %02x:%02x:%02x:%02x:%02x:%02x features=0x%llx\n",
              nic->mac[0], nic->mac[1], nic->mac[2],
              nic->mac[3], nic->mac[4], nic->mac[5],
              (unsigned long long)nic->negotiated_features);

    if (virtio_setup_queue(nic, &nic->rxq, VIRTIO_NET_Q_RX, "rx") != 0 ||
        virtio_setup_queue(nic, &nic->txq, VIRTIO_NET_Q_TX, "tx") != 0) {
        vn_write(nic, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return NULL;
    }

    virtio_net_refill_rx(nic);
    vn_write(nic, VIRTIO_MMIO_INTERRUPT_ACK, 0xffffffffU);
    virtio_status_or(nic, VIRTIO_STATUS_DRIVER_OK);
    nic->ready = 1;

    KLOG_INFO("[virtio-net] init done status=0x%x isr=0x%x rx_avail=%u tx_avail=%u\n",
              vn_read(nic, VIRTIO_MMIO_STATUS),
              vn_read(nic, VIRTIO_MMIO_INTERRUPT_STATUS),
              nic->rxq.avail.idx, nic->txq.avail.idx);

    g_eth0 = nic;

    static netdev_t virtio_dev;
    memset(&virtio_dev, 0, sizeof(virtio_dev));
    virtio_dev.name = "virtio0";
    virtio_dev.ctx = nic;
    virtio_dev.send = virtio_netdev_send;
    virtio_dev.recv = virtio_netdev_recv;
    memcpy(virtio_dev.mac, nic->mac, 6);
    netdev_register(&virtio_dev);

    return nic;
}

int eth_send(VirtioNetNic_t *opaque, const uint8_t *data, size_t len)
{
    struct virtio_net_nic *nic = opaque;
    if (!nic || !nic->ready || !data || len == 0U || len > ETH_FRAME_MAX)
        return -2;

    virtio_net_reclaim_tx(nic);

    int hdr_id = virtq_alloc_desc(&nic->txq);
    int frame_id = virtq_alloc_desc(&nic->txq);
    if (hdr_id < 0 || frame_id < 0) {
        if (hdr_id >= 0)
            virtq_free_desc(&nic->txq, (uint16_t)hdr_id);
        if (frame_id >= 0)
            virtq_free_desc(&nic->txq, (uint16_t)frame_id);
        nic->tx_busy_count++;
        KLOG_DEBUG("[virtio-net] tx busy len=%zu busy_count=%llu\n",
                   len, (unsigned long long)nic->tx_busy_count);
        return -1;
    }

    size_t frame_len = len < ETH_FRAME_MIN ? ETH_FRAME_MIN : len;
    uint16_t buf_id = (uint16_t)(hdr_id % VIRTIO_NET_TX_BUFS);
    virtio_net_buf_t *buf = &nic->tx_bufs[buf_id];
    memset(buf, 0, sizeof(*buf));
    memcpy(buf->frame, data, len);

    nic->txq.desc[hdr_id].addr = (uint64_t)virtio_dma_addr(buf->hdr);
    nic->txq.desc[hdr_id].len = VIRTIO_NET_HDR_LEN;
    nic->txq.desc[hdr_id].flags = VRING_DESC_F_NEXT;
    nic->txq.desc[hdr_id].next = (uint16_t)frame_id;

    nic->txq.desc[frame_id].addr = (uint64_t)virtio_dma_addr(buf->frame);
    nic->txq.desc[frame_id].len = (uint32_t)frame_len;
    nic->txq.desc[frame_id].flags = 0;
    nic->txq.desc[frame_id].next = 0;

    nic->tx_busy[hdr_id] = 1;
    nic->tx_head_to_buf[hdr_id] = buf_id;
    virtq_push_avail(nic, &nic->txq, VIRTIO_NET_Q_TX, (uint16_t)hdr_id);
    nic->tx_packets++;

    KLOG_DEBUG("[virtio-net] tx submit hdr=%d frame=%d buf=%u frame_len=%zu wire_len=%u avail_idx=%u tx_packets=%llu dst=%02x:%02x:%02x:%02x:%02x:%02x ethertype=0x%02x%02x\n",
               hdr_id, frame_id, buf_id, len,
               (uint32_t)(VIRTIO_NET_HDR_LEN + frame_len), nic->txq.avail.idx,
               (unsigned long long)nic->tx_packets,
               data[0], data[1], data[2], data[3], data[4], data[5],
               len >= 14U ? data[12] : 0U, len >= 14U ? data[13] : 0U);
    return 0;
}

int eth_recv(VirtioNetNic_t *opaque, uint8_t *buf, size_t maxlen)
{
    struct virtio_net_nic *nic = opaque;
    if (!nic || !nic->ready || !buf || maxlen == 0U)
        return -1;

    rmb();
    if (nic->rxq.last_used_idx == nic->rxq.used.idx)
        return 0;

    virtq_used_elem_t *e =
        &nic->rxq.used.ring[nic->rxq.last_used_idx % nic->rxq.num];
    uint16_t id = (uint16_t)e->id;
    uint32_t used_len = e->len;
    nic->rxq.last_used_idx++;

    if (id >= VIRTIO_NET_RX_BUFS || used_len < VIRTIO_NET_HDR_LEN) {
        nic->rx_drops++;
        KLOG_WARN("[virtio-net] rx invalid id=%u used_len=%u drops=%llu\n",
                  id, used_len, (unsigned long long)nic->rx_drops);
        if (id < VIRTIO_NET_RX_BUFS)
            virtq_free_desc(&nic->rxq, id);
        virtio_net_refill_rx(nic);
        return -1;
    }

    virtio_net_buf_t *rx = &nic->rx_bufs[id];
    uint32_t frame_len = used_len - VIRTIO_NET_HDR_LEN;
    size_t copy_len = frame_len < maxlen ? frame_len : maxlen;
    memcpy(buf, rx->frame, copy_len);
    virtq_free_desc(&nic->rxq, id);
    virtio_net_refill_rx(nic);
    nic->rx_packets++;

    KLOG_DEBUG("[virtio-net] rx packet id=%u frame_len=%u copy=%zu last_used=%u rx_packets=%llu dst=%02x:%02x:%02x:%02x:%02x:%02x src=%02x:%02x:%02x:%02x:%02x:%02x type=0x%02x%02x\n",
               id, frame_len, copy_len, nic->rxq.last_used_idx,
               (unsigned long long)nic->rx_packets,
               copy_len >= 6U ? buf[0] : 0U, copy_len >= 6U ? buf[1] : 0U,
               copy_len >= 6U ? buf[2] : 0U, copy_len >= 6U ? buf[3] : 0U,
               copy_len >= 6U ? buf[4] : 0U, copy_len >= 6U ? buf[5] : 0U,
               copy_len >= 12U ? buf[6] : 0U, copy_len >= 12U ? buf[7] : 0U,
               copy_len >= 12U ? buf[8] : 0U, copy_len >= 12U ? buf[9] : 0U,
               copy_len >= 12U ? buf[10] : 0U, copy_len >= 12U ? buf[11] : 0U,
               copy_len >= 14U ? buf[12] : 0U, copy_len >= 14U ? buf[13] : 0U);
    return (int)copy_len;
}

void eth_mac_addr(VirtioNetNic_t *opaque, uint8_t mac[6])
{
    struct virtio_net_nic *nic = opaque;
    if (!mac)
        return;
    if (!nic) {
        memset(mac, 0, 6);
        return;
    }
    memcpy(mac, nic->mac, 6);
}

void virtio_net_init_from_platform(void)
{
    uintptr_t base = VIRTIO_NET_PLATFORM_BASE;
    KLOG_DEBUG("[virtio-net] platform eth.base raw=0x%lx mmio=0x%lx mmio_vma=%u\n",
               (unsigned long)(uintptr_t)DEVICE_ETH_BASE_RAW,
               (unsigned long)base, (unsigned)DEVICE_MMIO_NEEDS_VMA);
    if (base == 0) {
        KLOG_WARN("[virtio-net] no eth.base in platform config, skip init\n");
        return;
    }
    eth_init((uint64_t)base);
}

static void virtio_net_send_probe(struct virtio_net_nic *nic)
{
    uint8_t pkt[64];
    memset(pkt, 0, sizeof(pkt));
    for (uint32_t i = 0; i < 6U; i++)
        pkt[i] = 0xffU;
    memcpy(&pkt[6], nic->mac, 6);
    pkt[12] = 0x88U;
    pkt[13] = 0xb5U;
    const char payload[] = "avatar virtio-net probe";
    memcpy(&pkt[14], payload, sizeof(payload) - 1U);

    int rc = eth_send(nic, pkt, sizeof(pkt));
    KLOG_INFO("[virtio-net] probe tx rc=%d len=%zu\n", rc, sizeof(pkt));
}

void virtio_net_poll_demo_task(void *arg)
{
    (void)arg;
    struct virtio_net_nic *nic = g_eth0;
    if (!nic || !nic->ready) {
        KLOG_WARN("[virtio-net] poll task no ready NIC\n");
        task_exit();
    }

    KLOG_INFO("[virtio-net] poll task started mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
              nic->mac[0], nic->mac[1], nic->mac[2],
              nic->mac[3], nic->mac[4], nic->mac[5]);

    uint64_t last_probe_ms = 0;
    uint8_t rx[ETH_BUF_SIZE];
    for (;;) {
        int n = eth_recv(nic, rx, sizeof(rx));
        if (n > 0) {
            KLOG_INFO("[virtio-net] rx frame len=%d dst=%02x:%02x:%02x:%02x:%02x:%02x src=%02x:%02x:%02x:%02x:%02x:%02x type=0x%02x%02x\n",
                      n,
                      rx[0], rx[1], rx[2], rx[3], rx[4], rx[5],
                      rx[6], rx[7], rx[8], rx[9], rx[10], rx[11],
                      n >= 14 ? rx[12] : 0U, n >= 14 ? rx[13] : 0U);
        }

        uint64_t now = timer_get_uptime_ms();
        if (now - last_probe_ms >= 1000U) {
            virtio_net_send_probe(nic);
            last_probe_ms = now;
        }
        task_yield();
    }
}
