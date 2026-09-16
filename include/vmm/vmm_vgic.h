/*
 * include/vmm_vgic.h - VM-owned virtual GICv2 state and core operations.
 *
 * The VM owns one vgic_t. Distributor state is shared by the VM, while SGI,
 * PPI, GICC and GICH state is banked per virtual CPU.
 */
#ifndef VMM_VGIC_H
#define VMM_VGIC_H

#include "types.h"

#define VGIC_MAX_IRQS   1024
#define VGIC_MAX_WORDS  (VGIC_MAX_IRQS / 32)
#define VGIC_MAX_VCPUS  8
#define VGIC_MAX_LRS    4
#define VGICD_REG_SIZE  0x1000

#define LR_HW              (1u << 31)
#define LR_GROUP1         (1u << 30)
#define LR_STATE_PENDING  (1u << 28)
#define LR_PRIORITY       (0x14u << 23)
#define LR_STATE_MASK     (0x3u << 28)
#define LR_SGI_SRC_SHIFT  10
#define LR_VINTID_MASK    0x3ffu

typedef struct vgic_vcpu_state {
    /* Banked SGI/PPI state for IRQs 0..31. SGIs are always enabled here. */
    uint32_t enabled0;
    uint32_t pending0;
    uint32_t active0;
    uint16_t sgi_sources[16];

    /* Guest GICC state. */
    uint32_t gicc_ctlr;
    uint32_t gicc_pmr;
    uint32_t gicc_iidr;
    uint32_t gicc_eoi_mode;

    /* Cached hardware GICH list-register state. */
    uint32_t lr[VGIC_MAX_LRS];
} vgic_vcpu_state_t;

typedef struct vgic {
    uint32_t nr_vcpus;
    int dist_enabled;

    /* Distributor-wide SPI state. */
    uint32_t enabled[VGIC_MAX_WORDS];
    uint32_t spi_pending[VGIC_MAX_VCPUS][VGIC_MAX_WORDS];
    uint32_t spi_active[VGIC_MAX_VCPUS][VGIC_MAX_WORDS];
    uint8_t  dist_regs[VGICD_REG_SIZE];
    uint8_t  targets[VGIC_MAX_IRQS];

    vgic_vcpu_state_t vcpu[VGIC_MAX_VCPUS];
} vgic_t;

int vmm_vgic_init(vgic_t *vgic, uint32_t nr_vcpus);

void vmm_vgic_set_pending(vgic_t *vgic, uint32_t vcpu_id, uint32_t irq);
void vmm_vgic_set_sgi_pending(vgic_t *vgic, uint32_t vcpu_id,
                              uint32_t source_vcpu, uint32_t irq);
void vmm_vgic_set_enabled(vgic_t *vgic, uint32_t vcpu_id, uint32_t irq,
                          int enabled);

void vmm_vgic_set_dist_enabled(vgic_t *vgic, int enabled);
int  vmm_vgic_dist_enabled(const vgic_t *vgic);

uint32_t vmm_vgic_enabled_word(const vgic_t *vgic, uint32_t vcpu_id,
                               uint32_t word);
uint32_t vmm_vgic_pending_word(const vgic_t *vgic, uint32_t vcpu_id,
                               uint32_t word);
uint32_t vmm_vgic_active_word(const vgic_t *vgic, uint32_t vcpu_id,
                              uint32_t word);
void vmm_vgic_clear_pending_word(vgic_t *vgic, uint32_t vcpu_id,
                                 uint32_t word, uint32_t bits);
void vmm_vgic_clear_active_word(vgic_t *vgic, uint32_t vcpu_id,
                                uint32_t word, uint32_t bits);

void vmm_vgic_inject_timer(vgic_t *vgic, uint32_t vcpu_id);
void vmm_vgic_sync_entry(vgic_t *vgic, uint32_t vcpu_id);
void vmm_vgic_sync_exit(vgic_t *vgic, uint32_t vcpu_id);

int  vmm_vgic_next_pending(const vgic_t *vgic, uint32_t vcpu_id);
int  vmm_vgic_ack(vgic_t *vgic, uint32_t vcpu_id);
void vmm_vgic_eoi(vgic_t *vgic, uint32_t vcpu_id, uint32_t irq);

#endif /* VMM_VGIC_H */
