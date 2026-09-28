/*
 * kernel/vmm/fdt.c — FDT/DTB 的字节级工具与补丁
 *
 * 从 guest_loader.c 拆出来的。这一组的特点是**入参就是宿主缓冲**
 * （`uint8_t *dtb, uint32_t dtb_size`），一处不碰 vm_t、也不碰 stage-2 ——
 * 所以是全文件里唯一能零改动搬走的部分。
 *
 * 内容：
 *   - FDT 常量与 token 级工具（be32 / put_be32 / find_string_offset / encode_cells）
 *   - guest_loader_patch_dtb_initrd   —— 改写 /chosen/linux,initrd-{start,end}
 *   - guest_loader_patch_dtb_memory   —— 改写 /memory 的 reg
 *   - guest_loader_patch_dtb_bootargs —— 原地改写 /chosen/bootargs
 *   - guest_loader_nop_dtb_nodes      —— 把未模拟的设备节点改成 nop
 *
 * 原型都在 include/guest_loader.h（公共头），所以拆出来不需要动任何声明。
 */

#include "guest_loader.h"
#include "cache.h" /* clean_dcache_range：补丁改的是 guest 要读的内存 */
#include "klog.h"
#include "string.h"

/* ── FDT 常量与工具 ───────────────────────────────────────── */
#define FDT_MAGIC      0x00d00dfeedu
#define FDT_BEGIN_NODE 1u
#define FDT_END_NODE   2u
#define FDT_PROP       3u
#define FDT_NOP        4u
#define FDT_END        9u

static uint32_t be32(const uint8_t *p, uint32_t off)
{
    return ((uint32_t)p[off] << 24) | ((uint32_t)p[off + 1] << 16) |
           ((uint32_t)p[off + 2] << 8) | (uint32_t)p[off + 3];
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
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
            memcmp(strings + i, name, nlen) == 0 && strings[i + nlen] == 0)
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

/* ── DTB 修补 ─────────────────────────────────────────────── */
/*
 * 遍历 FDT 结构块，把 initrd 的 start/end 两个 64 位属性改写到指定值。
 * （对标 kvmm loader.rs 的 patch_dtb_initrd）
 */
int guest_loader_patch_dtb_initrd(uint8_t *dtb, uint32_t dtb_size,
                                  uint64_t initrd_start, uint64_t initrd_end)
{
    if (dtb_size < 40 || be32(dtb, 0) != FDT_MAGIC) {
        KLOG_ERROR("[guest] DTB bad magic\n");
        return -1;
    }

    uint32_t off_struct = be32(dtb, 8);
    uint32_t off_strings = be32(dtb, 12);
    uint32_t size_strings = be32(dtb, 32);

    if (off_strings + size_strings > dtb_size)
        return -1;

    int start_noff = find_string_offset(dtb + off_strings, size_strings,
                                        "linux,initrd-start");
    int end_noff =
        find_string_offset(dtb + off_strings, size_strings, "linux,initrd-end");
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
            break; /* FDT_END 或异常 */
        }
    }

    KLOG_INFO("[guest] DTB initrd patched: [0x%llx, 0x%llx) (%d props)\n",
              (unsigned long long)initrd_start, (unsigned long long)initrd_end,
              patched);
    if (patched > 0)
        clean_dcache_range(dtb, dtb_size);
    return patched > 0 ? 0 : -1;
}

/*
 * 改写 /memory 节点的 reg（base/size），使 guest 看到我们实际提供的大小。
 * cells 数取根节点 #address-cells/#size-cells（arm64 通常 2/2）。
 */
int guest_loader_patch_dtb_memory(uint8_t *dtb, uint32_t dtb_size,
                                  uint64_t mem_base, uint64_t mem_size)
{
    if (dtb_size < 40 || be32(dtb, 0) != FDT_MAGIC)
        return -1;

    uint32_t off_struct = be32(dtb, 8);
    uint32_t off_strings = be32(dtb, 12);
    uint32_t size_strings = be32(dtb, 32);

    if (off_strings + size_strings > dtb_size)
        return -1;

    int reg_noff = find_string_offset(dtb + off_strings, size_strings, "reg");
    int addr_cells_noff =
        find_string_offset(dtb + off_strings, size_strings, "#address-cells");
    int size_cells_noff =
        find_string_offset(dtb + off_strings, size_strings, "#size-cells");

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
                (addr_cells + size_cells) * 4 == len &&
                data + len <= dtb_size) {
                encode_cells(dtb + data, (int)addr_cells, mem_base);
                encode_cells(dtb + data + addr_cells * 4, (int)size_cells,
                             mem_size);
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
              (unsigned long long)mem_base, (unsigned long long)mem_size,
              patched);
    if (patched > 0)
        clean_dcache_range(dtb, dtb_size);
    return patched > 0 ? 0 : -1;
}

int guest_loader_patch_dtb_bootargs(uint8_t *dtb, uint32_t dtb_size,
                                    const char *bootargs)
{
    if (dtb_size < 40 || be32(dtb, 0) != FDT_MAGIC)
        return -1;

    uint32_t off_struct = be32(dtb, 8);
    uint32_t off_strings = be32(dtb, 12);
    uint32_t size_strings = be32(dtb, 32);

    if (off_strings + size_strings > dtb_size)
        return -1;

    int bootargs_noff =
        find_string_offset(dtb + off_strings, size_strings, "bootargs");
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
                    KLOG_WARN(
                        "[guest] DTB bootargs buffer too small: have=%u need=%llu\n",
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
            return -1; /* FDT_END 或异常 */
        }
    }
    return -1;
}

static void dtb_nop_range(uint8_t *dtb, uint32_t start, uint32_t end)
{
    for (uint32_t p = start; p + 4 <= end; p += 4)
        put_be32(dtb + p, FDT_NOP);
}

int guest_loader_nop_dtb_nodes(uint8_t *dtb, uint32_t dtb_size,
                               const char *const *names, int nr_names)
{
    if (dtb_size < 40 || be32(dtb, 0) != FDT_MAGIC)
        return -1;

    uint32_t off_struct = be32(dtb, 8);
    uint32_t size_struct = be32(dtb, 36);
    uint32_t end_struct = off_struct + size_struct;

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
