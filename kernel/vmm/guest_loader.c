/*
 * kernel/vmm/guest_loader.c — guest 镜像加载与启动
 *
 * 移植自 x-kernel: virt/kvmm-api/src/loader.rs，适配 Avatar OS：
 *   - kvfs → avatar 的 vfs_open / vfs_read_to_phys
 *   - 加载目标为 guest 物理地址（经 stage2 的 identity map 直接换算）
 *   - DTB 修补逻辑（initrd / memory）按 FDT 规范逐 token 走查
 */

#include "guest_loader.h"
#include "cache.h"
#include "klog.h"
#include "mm_vm.h"
#include "pmm.h"
#include "string.h"
#include "task/task.h"
#include "vfs.h"
#include "vmm/vmm.h"
#include "vmm/vmm_mmio.h"

#if ARCH_AARCH64
#include "aarch64/stage2.h"
/* guest 入口时 x0 的值（ARM64 boot 约定：DTB 物理地址）*/
volatile uint64_t g_guest_entry_x0;
#endif

/* 加载缓冲（静态，避免大栈占用）*/
#define LOAD_CHUNK 4096
static uint8_t g_load_buf[LOAD_CHUNK];

/* ── FDT 常量与工具 ───────────────────────────────────────── */
#define FDT_MAGIC 0x00d00dfeedu
#define FDT_BEGIN_NODE 1u
#define FDT_END_NODE 2u
#define FDT_PROP 3u
#define FDT_NOP 4u
#define FDT_END 9u

static uint32_t be32(const uint8_t *p, uint32_t off) {
  return ((uint32_t)p[off] << 24) | ((uint32_t)p[off + 1] << 16) |
         ((uint32_t)p[off + 2] << 8) | (uint32_t)p[off + 3];
}

static void put_be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

/* 在 strings 块里找以 NUL 结尾的属性名，返回其偏移 */
static int find_string_offset(const uint8_t *strings, uint32_t slen,
                              const char *name) {
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
static void encode_cells(uint8_t *p, int cells, uint64_t value) {
  for (int i = 0; i < cells; i++) {
    int shift = (cells - 1 - i) * 32;
    put_be32(p + i * 4, (uint32_t)(value >> shift));
  }
}

/* ── 文件加载 ─────────────────────────────────────────────── */
int guest_loader_load_file(const char *path, uint64_t gpa) {
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
      break; /* 读到文件尾 */
  }

  vfs_close(f);
  if (total > 0)
    clean_dcache_range(phys_to_virt(gpa), (size_t)total);
  KLOG_INFO("[guest] loaded '%s': %d bytes -> GPA 0x%llx\n", path, total,
            (unsigned long long)gpa);
  return total;
}

/* ── DTB 修补 ─────────────────────────────────────────────── */
/*
 * 遍历 FDT 结构块，把 initrd 的 start/end 两个 64 位属性改写到指定值。
 * （对标 kvmm loader.rs 的 patch_dtb_initrd）
 */
int guest_loader_patch_dtb_initrd(uint64_t dtb_gpa, uint32_t dtb_size,
                                  uint64_t initrd_start, uint64_t initrd_end) {
  uint8_t *dtb = (uint8_t *)phys_to_virt(dtb_gpa);

  if (dtb_size < 40 || be32(dtb, 0) != FDT_MAGIC) {
    KLOG_ERROR("[guest] DTB bad magic\n");
    return -1;
  }

  uint32_t off_struct = be32(dtb, 8);
  uint32_t off_strings = be32(dtb, 12);
  uint32_t size_strings = be32(dtb, 32);

  if (off_strings + size_strings > dtb_size)
    return -1;

  int start_noff =
      find_string_offset(dtb + off_strings, size_strings, "linux,initrd-start");
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
int guest_loader_patch_dtb_memory(uint64_t dtb_gpa, uint32_t dtb_size,
                                  uint64_t mem_base, uint64_t mem_size) {
  uint8_t *dtb = (uint8_t *)phys_to_virt(dtb_gpa);

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
            (unsigned long long)mem_base, (unsigned long long)mem_size,
            patched);
  if (patched > 0)
    clean_dcache_range(dtb, dtb_size);
  return patched > 0 ? 0 : -1;
}

int guest_loader_patch_dtb_bootargs(uint64_t dtb_gpa, uint32_t dtb_size,
                                    const char *bootargs) {
  uint8_t *dtb = (uint8_t *)phys_to_virt(dtb_gpa);

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
              "[guest] DTB bootargs buffer too small: have=%u need=%llu\n", len,
              (unsigned long long)new_len);
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
                        uint32_t *end_out) {
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

static void dtb_nop_range(uint8_t *dtb, uint32_t start, uint32_t end) {
  for (uint32_t p = start; p + 4 <= end; p += 4)
    put_be32(dtb + p, FDT_NOP);
}

int guest_loader_nop_dtb_nodes(uint64_t dtb_gpa, uint32_t dtb_size,
                               const char *const *names, int nr_names) {
  uint8_t *dtb = (uint8_t *)phys_to_virt(dtb_gpa);

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
        if (nlen == want && memcmp(dtb + name_start, names[i], want) == 0) {
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

/*
 * guest 内核命令行。
 *
 * ⚠️ 上限 78 字节（不含结尾 NUL）：DTB 里 /chosen/bootargs 属性只有 79 字节
 * 的槽位，而 guest_loader_patch_dtb_bootargs() 是**原地改写、不支持加长**。
 * 超了补丁就失败，guest 会退回 DTS 里那句 —— 表现和成功一模一样（照常启动），
 * 所以这个宏等于没生效。历史上就是这样：原串 86 字节，静默失败了很久，
 * 直到调 guest 启动速度时才发现。下面的 _Static_assert 让超长直接编译不过。
 *
 * 内容取舍：
 *   quiet          console_loglevel 7→4，只放行 ERR 及以上。
 *                  guest 每写一个字符到 UARTDR 都要陷入 EL2 做一次完整的
 *                  世界切换（PL011 在 stage-2 里是无效映射），实测 35~70µs
 *                  /字符。约 1.2 万字符的启动日志要花掉 0.86s —— 而裸跑同样
 *                  的日志只差 0.02s，因为那边没有 exit。quiet 只影响往串口写
 *                  的部分，内核环形缓冲里仍是完整的，事后 dmesg 全看得到。
 *   console=       guest 控制台走 PL011（与 vpl011 同地址 0x09000000）。
 *   rdinit=/init   显式指定 init，不依赖内核「没有 init= 就找 /init」的回退。
 *   panic_on_warn=0 / oops=panic  调试用：警告不停机，oops 停。
 *
 * 想恢复完整启动日志：去掉 "quiet "（还剩 55 字节，仍在预算内）。
 * 不带 earlycon：它和 quiet 同时开意义不大（早期消息同样被 loglevel 压掉），
 * 而且会让每条消息打印两遍（bootconsole + 真 console），白白多一倍 exit。
 */
#if ARCH_AARCH64
#define GUEST_LINUX_BOOTARGS                                                   \
  "quiet console=ttyAMA0 rdinit=/init panic_on_warn=0 oops=panic"

_Static_assert(sizeof(GUEST_LINUX_BOOTARGS) - 1 <= 78,
               "GUEST_LINUX_BOOTARGS 超出 DTB 的 /chosen/bootargs 槽位"
               "（79 字节含结尾 NUL），补丁会静默失败");

#elif ARCH_RISCV64
/*
 * RISC-V 侧同一个套路，但**槽位大得多**：imgs/guests/rv64/linux.dts 里
 * 那句自带命令行是详细日志版（90 字节），所以这里可以更宽松地在
 * 「详细 / quiet」之间切。默认对齐 aarch64 走 quiet。
 *
 * 内容取舍与 aarch64 相同（逐字符 UART 写都要陷一次 G-stage → MMIO 模拟，
 * 约 35~70µs/字符）：
 *   quiet          console_loglevel 7→4，只放行 ERR 及以上。
 *   console=       guest 控制台走 16550A（与 vuart16550 同地址 0x10000000）。
 *   rdinit=/init   显式指定 init，不依赖内核回退。
 *   panic_on_warn=0 / oops=panic  调试用：警告不停机，oops 停。
 *
 * 想恢复完整启动日志：去掉 "quiet "（还剩 32 字节，仍在预算内），想连
 * 8250 驱动接管之前的部分也看到就再加
 * `earlycon=uart8250,mmio,0x10000000`（代价是每条早期消息打两遍）。
 *
 * 注意：**补丁失败时会退回 DTB 里那句自带命令行**，而 imgs/guests/rv64/
 * linux.dts 里写的正是详细日志那一版 —— 所以「补丁静默失败」在这边表现得
 * 不是「少打日志」而是「日志变多」，反过来更好发现。
 */
#define GUEST_LINUX_BOOTARGS                                                   \
  " console=ttyS0 rdinit=/init panic_on_warn=0 oops=panic"

_Static_assert(sizeof(GUEST_LINUX_BOOTARGS) - 1 <= 90,
               "GUEST_LINUX_BOOTARGS 超出 DTB 的 /chosen/bootargs 槽位"
               "（91 字节含结尾 NUL），补丁会静默失败 —— 详见 "
               "imgs/guests/rv64/linux.dts 的注释（槽位由那句自带命令行决定）");
#endif

/* ── 启动 Linux guest ─────────────────────────────────────── */
/*
 * 调用者两种：
 *   - kernel_main（RUN_GUEST_LINUX 直启）：开机直接进 guest，没有宿主 shell；
 *   - /dev/vmm 的 bootlinux（宿主 shell 里跑 /bin/vmm-run）。
 *
 * 两个调用者靠 vm_t 是函数静态变量天然互斥，这里再加一道显式闸门：
 * 直启之后 shell 是不存在的，但反过来 —— 在 shell 里先启动过 guest、
 * 停掉、再启动 —— 会真的重入本函数。
 *
 * 重入是支持的：guest RAM 每次都被 memset 清干净、镜像重新加载、DTB 重新
 * 修补、vm_create() 重建 Stage-2 与 vGIC/vPL011。所以只需要挡住「上一个
 * guest 还在跑」这一种情况。
 */
#if ARCH_X86_64
/*
 * x86 的引导协议与 arm64/riscv 完全不同（bzImage + zeropage + E820 +
 * 长模式入口），而且本架构还有 GPA≠HPA 的额外一层。与其在下面这段
 * 共用的 DTB 逻辑里塞满 #if，不如整条路径单独实现、在这里分流 ——
 * 这样 arm64/riscv 那两段一行都不用动（回归风险为 0）。
 */
extern int x86_guest_boot(void);
#endif

int guest_loader_run_linux(void) {
#if ARCH_X86_64
  return x86_guest_boot();
#else
  static vm_t vm;
  int rc;

  if (vmm_guest_running()) {
    KLOG_WARN("[guest] a guest is already running, refusing re-entry\n");
    return -1;
  }

  KLOG_INFO("\n=== Avatar OS: booting Linux guest (%s) ===\n",
#if ARCH_AARCH64
            "aarch64"
#elif ARCH_RISCV64
            "riscv64"
#else
            "unknown"
#endif
  );

  /* The guest RAM range is identity-backed by host physical memory. Keep it
   * out of PMM before loading images or creating more host objects. */
  KLOG_INFO(
      "[guest] reserving RAM: 0x%llx - 0x%llx\n",
      (unsigned long long)GUEST_LINUX_MEM_BASE,
      (unsigned long long)(GUEST_LINUX_MEM_BASE + GUEST_LINUX_MEM_SIZE - 1));
  pmm_mark_allocated(g_pmm, GUEST_LINUX_MEM_BASE,
                     GUEST_LINUX_MEM_BASE + GUEST_LINUX_MEM_SIZE - 1);

  /* x-kernel backs guest RAM with freshly allocated zeroed pages. Avatar uses
   * a reserved physical window, so explicitly clear it before loading Linux;
   * otherwise stale host data can corrupt the guest's .bss/static tables. */
  memset((void *)phys_to_virt(GUEST_LINUX_MEM_BASE), 0, GUEST_LINUX_MEM_SIZE);
  clean_dcache_range((void *)phys_to_virt(GUEST_LINUX_MEM_BASE),
                     GUEST_LINUX_MEM_SIZE);

  /* 1. 加载 kernel Image */
  int klen =
      guest_loader_load_file(GUEST_LINUX_KERNEL_PATH, GUEST_LINUX_KERNEL_GPA);
  if (klen <= 0)
    return -1;
  KLOG_INFO("[guest] kernel: %d bytes\n", klen);

  /* 2. 加载 DTB，并修补 memory / initrd 节点 */
  int dlen = guest_loader_load_file(GUEST_LINUX_DTB_PATH, GUEST_LINUX_DTB_GPA);
  if (dlen <= 0)
    return -1;

  guest_loader_patch_dtb_memory(GUEST_LINUX_DTB_GPA, (uint32_t)dlen,
                                GUEST_LINUX_MEM_BASE, GUEST_LINUX_MEM_SIZE);

  if (guest_loader_patch_dtb_bootargs(GUEST_LINUX_DTB_GPA, (uint32_t)dlen,
                                      GUEST_LINUX_BOOTARGS) != 0) {
    KLOG_ERROR("[guest] bootargs 补丁失败：guest 会用 DTB 自带的那句命令行，"
               "GUEST_LINUX_BOOTARGS 不生效（原因见上一条 WARN）\n");
  }

  /* 3. 加载 initrd */
  int ilen =
      guest_loader_load_file(GUEST_LINUX_INITRD_PATH, GUEST_LINUX_INITRD_GPA);
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
    static const char *const unsupported_nodes[] =
        GUEST_LINUX_UNSUPPORTED_NODES;
    guest_loader_nop_dtb_nodes(
        GUEST_LINUX_DTB_GPA, (uint32_t)dlen, unsupported_nodes,
        (int)(sizeof(unsupported_nodes) / sizeof(unsupported_nodes[0])));
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

  /*
   * 5. 配置 vCPU 的入口状态
   *
   * 必须在 vm_create() **之后**：两个架构的 vm_init 都会 memset 整个
   * vcpu 数组，先写会被清掉。
   */
  vcpu_t *vcpu = &vm.vcpus[0];

#if ARCH_AARCH64
  /*
   * x0 直接写 vcpu->r[0]：el2_vmcs.S 在 eret 前用 `ldr x0, [x0, #0]`
   * 最后加载 guest x0（VCPU_R0 偏移 0），因此无需改动汇编。
   * ARM64 boot 约定：x0 = DTB 物理地址，PSTATE = EL1h 且 DAIF 屏蔽。
   */
  vcpu->elr = GUEST_LINUX_KERNEL_GPA;
  vcpu->spsr = 0x5ULL | (0xFULL << 6); /* EL1h, DAIF 屏蔽 */
  vcpu->sp_el1 = GUEST_LINUX_MEM_BASE + GUEST_LINUX_MEM_SIZE - 0x1000;
  vcpu->r[0] = GUEST_LINUX_DTB_GPA;
  g_guest_entry_x0 = GUEST_LINUX_DTB_GPA;

  KLOG_INFO("[guest] entry=0x%llx x0(DTB)=0x%llx sp_el1=0x%llx\n",
            (unsigned long long)vcpu->elr,
            (unsigned long long)GUEST_LINUX_DTB_GPA,
            (unsigned long long)vcpu->sp_el1);

#elif ARCH_RISCV64
  /*
   * RISC-V Linux boot 协议（Documentation/riscv/boot.rst）：
   *   pc = Image 起始（按 2 MiB 对齐装入）
   *   a0 = boot hartid
   *   a1 = DTB 物理地址
   * 其余寄存器无约定；sp 由内核自己在 head.S 里设好，这里给个合法值
   * 只是为了「万一它在设 sp 之前先出异常」时不至于落到 0。
   *
   * vsatp = 0（bare）：**不能**沿用宿主 satp —— guest 是独立地址空间，
   * 由 Linux 自己在 head.S 里建页表再写 satp。沿用宿主页表的话，guest
   * 一取指就会按宿主的内核映射跑，行为完全不可预期。
   *
   * vstvec 也留 0：Linux 在 head.S 很早就会 csrw stvec 换成自己的
   * trap vector；在此之前不让任何中断挂起（vsie=0，hvip 的注入发生在
   * 每次入口、由 vmm_arch_restore_guest_ctx 计算）。
   */
  vcpu->pc = GUEST_LINUX_KERNEL_GPA;    /* 恢复 PC = sret 目标 */
  vcpu->vsepc = GUEST_LINUX_KERNEL_GPA; /* guest 自己的 sepc   */
  vcpu->vsatp = 0;
  vcpu->vsstatus = 0;
  vcpu->vstvec = 0;
  vcpu->vsie = 0;
  vcpu->vsscratch = 0;
  vcpu->r[2] = GUEST_LINUX_MEM_BASE + GUEST_LINUX_MEM_SIZE - 0x1000;
  vcpu->r[10] = GUEST_LINUX_BOOT_HARTID; /* a0 */
  vcpu->r[11] = GUEST_LINUX_DTB_GPA;     /* a1 */

  KLOG_INFO("[guest] entry=0x%llx a0(hartid)=%llu a1(DTB)=0x%llx\n",
            (unsigned long long)vcpu->vsepc, (unsigned long long)vcpu->r[10],
            (unsigned long long)vcpu->r[11]);
#endif

  /* 6. 创建 vCPU 任务 */
  task_t *vt = vcpu_task_create(vcpu, 5);
  if (!vt) {
    KLOG_ERROR("[guest] vcpu_task_create failed\n");
    return -1;
  }
  KLOG_INFO("[guest] Linux vCPU task id=%u running\n", vt->id);
  return 0;
#endif /* !ARCH_X86_64 */
}
