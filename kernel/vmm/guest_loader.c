/*
 * kernel/vmm/guest_loader.c — guest 镜像加载与启动
 *
 * 移植自 x-kernel: virt/kvmm-api/src/loader.rs，适配 Avatar OS：
 *   - kvfs → avatar 的 vfs_open / vfs_read_to_phys
 *   - 加载目标为 guest 物理地址（经 stage2 的 identity map 直接换算）
 *   - DTB 修补逻辑（initrd / memory）按 FDT 规范逐 token 走查
 */

#include "guest_loader.h"
#include "vmm.h"
#include "vmm_mmio.h"
#include "vmm_vpl011.h"
#include "vmm_vgicd.h"
#include "vmm_vgic.h"
#include "vfs.h"
#include "mm_vm.h"
#include "pmm.h"
#include "klog.h"
#include "string.h"
#include "cache.h"
#include "task/task.h"
#include "aarch64/stage2.h"

/* guest 入口时 x0 的值（ARM64 boot 约定：DTB 物理地址）*/
volatile uint64_t g_guest_entry_x0;

/* 加载缓冲（静态，避免大栈占用）*/
#define LOAD_CHUNK  4096
static uint8_t g_load_buf[LOAD_CHUNK];

/* ── FDT 常量与工具 ───────────────────────────────────────── */
#define FDT_MAGIC       0x00d00dfeedu
#define FDT_BEGIN_NODE  1u
#define FDT_END_NODE    2u
#define FDT_PROP        3u
#define FDT_NOP         4u
#define FDT_END         9u

static uint32_t be32(const uint8_t *p, uint32_t off)
{
    return ((uint32_t)p[off] << 24) | ((uint32_t)p[off + 1] << 16) |
           ((uint32_t)p[off + 2] << 8) | (uint32_t)p[off + 3];
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* 在 strings 块里找以 NUL 结尾的属性名，返回其偏移 */
static int find_string_offset(const uint8_t *strings, uint32_t slen,
                              const char *name)
{
    uint32_t nlen = (uint32_t)strlen(name);

    if (slen < nlen + 1)
        return -1;

    for (uint32_t i = 0; i + nlen < slen; i++) {
        if ((i == 0 || strings[i - 1] == 0) &&
            memcmp(strings + i, name, nlen) == 0 &&
            strings[i + nlen] == 0)
            return (int)i;
    }
    return -1;
}

/* 按 cells 个数以大端写入 value */
static void encode_cells(uint8_t *p, int cells, uint64_t value)
{
    for (int i = 0; i < cells; i++) {
        int shift = (cells - 1 - i) * 32;
        put_be32(p + i * 4, (uint32_t)(value >> shift));
    }
}

/* ── 文件加载 ─────────────────────────────────────────────── */
int guest_loader_load_file(const char *path, uint64_t gpa)
{
    vfs_file_t *f = NULL;
    uint64_t off = 0;
    int total = 0;
    int rc = vfs_open(path, 0 /*O_RDONLY*/, 0, &f);

    if (rc != 0 || !f) {
        KLOG_ERROR("[guest] cannot open '%s' (rc=%d)\n", path, rc);
        return -1;
    }

    for (;;) {
        int n = vfs_read_to_phys(f, off, g_load_buf, sizeof(g_load_buf));
        if (n <= 0)
            break;

        /* guest 物理地址 → 内核可写地址（stage2 identity map，GPA==PA）*/
        memcpy(phys_to_virt(gpa + off), g_load_buf, (size_t)n);

        off += (uint64_t)n;
        total += n;

        if (n < (int)sizeof(g_load_buf))
            break;          /* 读到文件尾 */
    }

    vfs_close(f);
    if (total > 0)
        clean_dcache_range(phys_to_virt(gpa), (size_t)total);
    KLOG_INFO("[guest] loaded '%s': %d bytes -> GPA 0x%llx\n",
              path, total, (unsigned long long)gpa);
    return total;
}

/* ── DTB 修补 ─────────────────────────────────────────────── */
/*
 * 遍历 FDT 结构块，把 initrd 的 start/end 两个 64 位属性改写到指定值。
 * （对标 kvmm loader.rs 的 patch_dtb_initrd）
 */
int guest_loader_patch_dtb_initrd(uint64_t dtb_gpa, uint32_t dtb_size,
                                  uint64_t initrd_start, uint64_t initrd_end)
{
    uint8_t *dtb = (uint8_t *)phys_to_virt(dtb_gpa);

    if (dtb_size < 40 || be32(dtb, 0) != FDT_MAGIC) {
        KLOG_ERROR("[guest] DTB bad magic\n");
        return -1;
    }

    uint32_t off_struct  = be32(dtb, 8);
    uint32_t off_strings = be32(dtb, 12);
    uint32_t size_strings = be32(dtb, 32);

    if (off_strings + size_strings > dtb_size)
        return -1;

    int start_noff = find_string_offset(dtb + off_strings, size_strings,
                                        "linux,initrd-start");
    int end_noff   = find_string_offset(dtb + off_strings, size_strings,
                                        "linux,initrd-end");
    if (start_noff < 0 && end_noff < 0) {
        KLOG_WARN("[guest] DTB has no initrd properties\n");
        return -1;
    }

    uint32_t pos = off_struct;
    int patched = 0;

    while (pos + 4 <= dtb_size) {
        uint32_t token = be32(dtb, pos);
        pos += 4;

        if (token == FDT_BEGIN_NODE) {
            while (pos < dtb_size && dtb[pos] != 0)
                pos++;
            pos++;
            pos = (pos + 3) & ~3u;
        } else if (token == FDT_PROP) {
            if (pos + 8 > dtb_size)
                break;
            uint32_t len = be32(dtb, pos);
            uint32_t nameoff = be32(dtb, pos + 4);
            uint32_t data = pos + 8;

            if ((len == 4 || len == 8) && data + len <= dtb_size) {
                if ((int)nameoff == start_noff) {
                    encode_cells(dtb + data, len == 8 ? 2 : 1, initrd_start);
                    patched++;
                } else if ((int)nameoff == end_noff) {
                    encode_cells(dtb + data, len == 8 ? 2 : 1, initrd_end);
                    patched++;
                }
            }
            pos = data + ((len + 3) & ~3u);
        } else if (token == FDT_END_NODE || token == FDT_NOP) {
            /* 无操作 */
        } else {
            break;      /* FDT_END 或异常 */
        }
    }

    KLOG_INFO("[guest] DTB initrd patched: [0x%llx, 0x%llx) (%d props)\n",
              (unsigned long long)initrd_start,
              (unsigned long long)initrd_end, patched);
    if (patched > 0)
        clean_dcache_range(dtb, dtb_size);
    return patched > 0 ? 0 : -1;
}

/*
 * 改写 /memory 节点的 reg（base/size），使 guest 看到我们实际提供的大小。
 * cells 数取根节点 #address-cells/#size-cells（arm64 通常 2/2）。
 */
int guest_loader_patch_dtb_memory(uint64_t dtb_gpa, uint32_t dtb_size,
                                  uint64_t mem_base, uint64_t mem_size)
{
    uint8_t *dtb = (uint8_t *)phys_to_virt(dtb_gpa);

    if (dtb_size < 40 || be32(dtb, 0) != FDT_MAGIC)
        return -1;

    uint32_t off_struct  = be32(dtb, 8);
    uint32_t off_strings = be32(dtb, 12);
    uint32_t size_strings = be32(dtb, 32);

    if (off_strings + size_strings > dtb_size)
        return -1;

    int reg_noff        = find_string_offset(dtb + off_strings, size_strings, "reg");
    int addr_cells_noff = find_string_offset(dtb + off_strings, size_strings, "#address-cells");
    int size_cells_noff = find_string_offset(dtb + off_strings, size_strings, "#size-cells");

    uint32_t addr_cells = 2, size_cells = 2;
    uint32_t depth = 0, memory_depth = 0;
    int in_memory = 0, patched = 0;

    uint32_t pos = off_struct;

    while (pos + 4 <= dtb_size) {
        uint32_t token = be32(dtb, pos);
        pos += 4;

        if (token == FDT_BEGIN_NODE) {
            uint32_t name_start = pos;
            while (pos < dtb_size && dtb[pos] != 0)
                pos++;
            uint32_t nlen = pos - name_start;
            pos++;
            pos = (pos + 3) & ~3u;

            depth++;
            if (depth == 2 && nlen >= 6 &&
                memcmp(dtb + name_start, "memory", 6) == 0) {
                in_memory = 1;
                memory_depth = depth;
            }
        } else if (token == FDT_PROP) {
            if (pos + 8 > dtb_size)
                break;
            uint32_t len = be32(dtb, pos);
            uint32_t nameoff = be32(dtb, pos + 4);
            uint32_t data = pos + 8;

            if (depth == 1 && len == 4 && data + 4 <= dtb_size) {
                if ((int)nameoff == addr_cells_noff)
                    addr_cells = be32(dtb, data);
                else if ((int)nameoff == size_cells_noff)
                    size_cells = be32(dtb, data);
            }

            if (in_memory && (int)nameoff == reg_noff &&
                (addr_cells + size_cells) * 4 == len && data + len <= dtb_size) {
                encode_cells(dtb + data, (int)addr_cells, mem_base);
                encode_cells(dtb + data + addr_cells * 4, (int)size_cells, mem_size);
                patched++;
            }

            pos = data + ((len + 3) & ~3u);
        } else if (token == FDT_END_NODE) {
            if (in_memory && depth == memory_depth)
                in_memory = 0;
            if (depth > 0)
                depth--;
        } else if (token == FDT_NOP) {
            /* 无操作 */
        } else {
            break;
        }
    }

    KLOG_INFO("[guest] DTB memory patched: base=0x%llx size=0x%llx (%d)\n",
              (unsigned long long)mem_base,
              (unsigned long long)mem_size, patched);
    if (patched > 0)
        clean_dcache_range(dtb, dtb_size);
    return patched > 0 ? 0 : -1;
}

static int guest_loader_patch_dtb_bootargs(uint64_t dtb_gpa, uint32_t dtb_size,
                                           const char *bootargs)
{
    uint8_t *dtb = (uint8_t *)phys_to_virt(dtb_gpa);

    if (dtb_size < 40 || be32(dtb, 0) != FDT_MAGIC)
        return -1;

    uint32_t off_struct   = be32(dtb, 8);
    uint32_t off_strings  = be32(dtb, 12);
    uint32_t size_strings = be32(dtb, 32);

    if (off_strings + size_strings > dtb_size)
        return -1;

    int bootargs_noff = find_string_offset(dtb + off_strings, size_strings,
                                           "bootargs");
    if (bootargs_noff < 0)
        return -1;

    uint32_t pos = off_struct;
    size_t new_len = strlen(bootargs) + 1;

    while (pos + 4 <= dtb_size) {
        uint32_t token = be32(dtb, pos);
        pos += 4;

        if (token == FDT_BEGIN_NODE) {
            while (pos < dtb_size && dtb[pos] != 0)
                pos++;
            pos++;
            pos = (pos + 3) & ~3u;
        } else if (token == FDT_PROP) {
            if (pos + 8 > dtb_size)
                break;
            uint32_t len = be32(dtb, pos);
            uint32_t nameoff = be32(dtb, pos + 4);
            uint32_t data = pos + 8;

            if ((int)nameoff == bootargs_noff && data + len <= dtb_size) {
                if (new_len > len) {
                    KLOG_WARN("[guest] DTB bootargs buffer too small: have=%u need=%llu\n",
                              len, (unsigned long long)new_len);
                    return -1;
                }
                memset(dtb + data, 0, len);
                memcpy(dtb + data, bootargs, new_len);
                clean_dcache_range(dtb, dtb_size);
                KLOG_INFO("[guest] DTB bootargs patched: %s\n", bootargs);
                return 0;
            }
            pos = data + ((len + 3) & ~3u);
        } else if (token == FDT_END_NODE || token == FDT_NOP) {
            /* 无操作 */
        } else {
            break;
        }
    }

    return -1;
}

/* ── 屏蔽 DTB 中未模拟的设备节点 ───────────────────────────── */
/*
 * 对标 kvmm loader.rs 的 nop_dtb_nodes：
 * 把指定节点的结构块子树整体替换为 FDT_NOP 令牌，使 guest 完全看不到
 * 该设备，从而不会去探测它。
 *
 * 为什么必须这么做：VMM 只模拟了 PL011 与 GICD，DTB 里其余设备
 * （GICv2M MSI 帧、PCIe、PL061、PL031、flash…）都没有实现。guest 一旦
 * 访问这些地址就会 Stage-2 fault，而 exit handler 未命中 MMIO 总线时按
 * 「权限故障」处理（恢复属性让 guest 重试）→ 立即再次 fault → **死循环**，
 * 表现为 guest 卡死在探测那台设备上。
 */
static int dtb_node_end(const uint8_t *dtb, uint32_t pos, uint32_t end,
                        uint32_t *end_out)
{
    uint32_t depth = 1;

    while (pos + 4 <= end) {
        uint32_t token = be32(dtb, pos);
        pos += 4;

        if (token == FDT_BEGIN_NODE) {
            while (pos < end && dtb[pos] != 0)
                pos++;
            if (pos >= end)
                return -1;
            pos++;
            pos = (pos + 3) & ~3u;
            depth++;
        } else if (token == FDT_PROP) {
            if (pos + 8 > end)
                return -1;
            uint32_t len = be32(dtb, pos);
            pos += 8 + ((len + 3) & ~3u);
            if (pos > end)
                return -1;
        } else if (token == FDT_END_NODE) {
            depth--;
            if (depth == 0) {
                *end_out = pos;
                return 0;
            }
        } else if (token == FDT_NOP) {
            /* 无操作 */
        } else {
            return -1;      /* FDT_END 或异常 */
        }
    }
    return -1;
}

static void dtb_nop_range(uint8_t *dtb, uint32_t start, uint32_t end)
{
    for (uint32_t p = start; p + 4 <= end; p += 4)
        put_be32(dtb + p, FDT_NOP);
}

int guest_loader_nop_dtb_nodes(uint64_t dtb_gpa, uint32_t dtb_size,
                               const char *const *names, int nr_names)
{
    uint8_t *dtb = (uint8_t *)phys_to_virt(dtb_gpa);

    if (dtb_size < 40 || be32(dtb, 0) != FDT_MAGIC)
        return -1;

    uint32_t off_struct   = be32(dtb, 8);
    uint32_t size_struct  = be32(dtb, 36);
    uint32_t end_struct   = off_struct + size_struct;

    if (end_struct > dtb_size)
        return -1;

    uint32_t pos = off_struct;
    int patched = 0;

    while (pos + 4 <= end_struct) {
        uint32_t token_pos = pos;
        uint32_t token = be32(dtb, pos);
        pos += 4;

        if (token == FDT_BEGIN_NODE) {
            uint32_t name_start = pos;
            while (pos < end_struct && dtb[pos] != 0)
                pos++;
            if (pos >= end_struct)
                break;
            uint32_t nlen = pos - name_start;
            pos++;
            pos = (pos + 3) & ~3u;

            /* 与待屏蔽名单逐个比对（按完整节点名）*/
            for (int i = 0; i < nr_names; i++) {
                uint32_t want = (uint32_t)strlen(names[i]);
                if (nlen == want &&
                    memcmp(dtb + name_start, names[i], want) == 0) {
                    uint32_t node_end;
                    if (dtb_node_end(dtb, pos, end_struct, &node_end) == 0) {
                        dtb_nop_range(dtb, token_pos, node_end);
                        patched++;
                        pos = node_end;
                    }
                    break;
                }
            }
        } else if (token == FDT_PROP) {
            if (pos + 8 > end_struct)
                break;
            uint32_t len = be32(dtb, pos);
            pos += 8 + ((len + 3) & ~3u);
        } else if (token == FDT_END_NODE || token == FDT_NOP) {
            /* 无操作 */
        } else {
            break;
        }
    }

    KLOG_INFO("[guest] DTB: %d device node(s) disabled\n", patched);
    if (patched > 0)
        clean_dcache_range(dtb, dtb_size);
    return patched;
}

/* ── 启动 Linux guest ─────────────────────────────────────── */
int guest_loader_run_linux(void)
{
    static vm_t vm;
    int rc;

    KLOG_INFO("\n=== Avatar OS: booting Linux guest (aarch64) ===\n");

    /* The guest RAM range is identity-backed by host physical memory. Keep it
     * out of PMM before loading images or creating more host objects. */
    KLOG_INFO("[guest] reserving RAM: 0x%llx - 0x%llx\n",
              (unsigned long long)GUEST_LINUX_MEM_BASE,
              (unsigned long long)(GUEST_LINUX_MEM_BASE +
                                   GUEST_LINUX_MEM_SIZE - 1));
    pmm_mark_allocated(g_pmm, GUEST_LINUX_MEM_BASE,
                       GUEST_LINUX_MEM_BASE + GUEST_LINUX_MEM_SIZE - 1);

    /* x-kernel backs guest RAM with freshly allocated zeroed pages. Avatar uses
     * a reserved physical window, so explicitly clear it before loading Linux;
     * otherwise stale host data can corrupt the guest's .bss/static tables. */
    memset((void *)phys_to_virt(GUEST_LINUX_MEM_BASE), 0,
           GUEST_LINUX_MEM_SIZE);
    clean_dcache_range((void *)phys_to_virt(GUEST_LINUX_MEM_BASE),
                       GUEST_LINUX_MEM_SIZE);

    /* 1. 加载 kernel Image */
    int klen = guest_loader_load_file(GUEST_LINUX_KERNEL_PATH,
                                      GUEST_LINUX_KERNEL_GPA);
    if (klen <= 0)
        return -1;
    KLOG_INFO("[guest] kernel: %d bytes\n", klen);

    /* 2. 加载 DTB，并修补 memory / initrd 节点 */
    int dlen = guest_loader_load_file(GUEST_LINUX_DTB_PATH,
                                      GUEST_LINUX_DTB_GPA);
    if (dlen <= 0)
        return -1;

    guest_loader_patch_dtb_memory(GUEST_LINUX_DTB_GPA, (uint32_t)dlen,
                                  GUEST_LINUX_MEM_BASE, GUEST_LINUX_MEM_SIZE);

    guest_loader_patch_dtb_bootargs(GUEST_LINUX_DTB_GPA, (uint32_t)dlen,
                                    "rdinit=/init console=ttyAMA0 earlycon=pl011,mmio,0x9000000 panic_on_warn=0 oops=panic");

    /* 3. 加载 initrd */
    int ilen = guest_loader_load_file(GUEST_LINUX_INITRD_PATH,
                                      GUEST_LINUX_INITRD_GPA);
    if (ilen <= 0)
        return -1;

    guest_loader_patch_dtb_initrd(GUEST_LINUX_DTB_GPA, (uint32_t)dlen,
                                  GUEST_LINUX_INITRD_GPA,
                                  GUEST_LINUX_INITRD_GPA + (uint64_t)ilen);

    /* ── 屏蔽 VMM 未模拟的 DTB 设备节点 ─────────────────────
     *
     * VMM 只模拟了 PL011 与 GICD。其余设备一旦被 guest 访问就会 Stage-2
     * fault，而 exit handler 在 MMIO 总线未命中时按「权限故障」处理
     * （stage2_restore 后让 guest 重试）→ 立刻再次 fault → **死循环**。
     *
     * 实测：guest 的 GIC 初始化会探测 "v2m@8020000"（GICv2M MSI 帧），
     * 因未模拟而卡死在 "Root IRQ handler: gic_handle_irq" 之后。
     *
     * 故启动前直接把这些节点从 DTB 中摘除（替换为 FDT_NOP），使 guest
     * 根本不去枚举它们。移植自 kvmm loader.rs 的 nop_dtb_nodes。*/
    {
        static const char *const unsupported_nodes[] = {
            "v2m@8020000",      /* GICv2M MSI 帧（导致 GIC 初始化卡死）*/
            "virtio_mmio@a000000", /* virtio-mmio transport（当前未模拟）*/
            "pcie@10000000",    /* PCIe ECAM */
            "pl061@9030000",    /* GPIO */
            "pl031@9010000",    /* RTC */
            "flash@0",          /* CFI flash */
            "fw-cfg@9020000",   /* QEMU fw_cfg */
        };
        guest_loader_nop_dtb_nodes(GUEST_LINUX_DTB_GPA, (uint32_t)dlen,
                                   unsupported_nodes,
                                   (int)(sizeof(unsupported_nodes) /
                                         sizeof(unsupported_nodes[0])));
    }

    /* 4. 建立 VM（启用 Stage-2 隔离与 MMIO 设备）*/
    vm.cfg.mem_base = GUEST_LINUX_MEM_BASE;
    vm.cfg.mem_size = GUEST_LINUX_MEM_SIZE;
    vm.cfg.nr_vcpus = 1;

    rc = vm_create(&vm);
    if (rc != 0) {
        KLOG_ERROR("[guest] vm_create failed\n");
        return -1;
    }

    /* 5. 配置 vCPU：入口 = kernel Image 起始，x0 = DTB 物理地址
     *
     * x0 直接写 vcpu->r[0]：el2_vmcs.S 在 eret 前用 `ldr x0, [x0, #0]`
     * 最后加载 guest x0（VCPU_R0 偏移 0），因此无需改动汇编。
     * ARM64 boot 约定：x0 = DTB 物理地址，PSTATE = EL1h 且 DAIF 屏蔽。*/
    vcpu_t *vcpu = &vm.vcpus[0];
    vcpu->elr    = GUEST_LINUX_KERNEL_GPA;
    vcpu->spsr   = 0x5ULL | (0xFULL << 6);   /* EL1h, DAIF 屏蔽 */
    vcpu->sp_el1 = GUEST_LINUX_MEM_BASE + GUEST_LINUX_MEM_SIZE - 0x1000;
    vcpu->r[0]   = GUEST_LINUX_DTB_GPA;
    g_guest_entry_x0 = GUEST_LINUX_DTB_GPA;

    KLOG_INFO("[guest] entry=0x%llx x0(DTB)=0x%llx sp_el1=0x%llx\n",
              (unsigned long long)vcpu->elr,
              (unsigned long long)GUEST_LINUX_DTB_GPA,
              (unsigned long long)vcpu->sp_el1);

    /* 6. 创建 vCPU 任务 */
    task_t *vt = vcpu_task_create(vcpu, 5);
    if (!vt) {
        KLOG_ERROR("[guest] vcpu_task_create failed\n");
        return -1;
    }
    KLOG_INFO("[guest] Linux vCPU task id=%u running\n", vt->id);
    return 0;
}
