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
#elif ARCH_RISCV64
#include "riscv64/gstage.h"
#elif ARCH_X86_64
#include "x86_64/ept.h"
#endif

/*
 * 每 VM 的静态表槽位数必须装得下 guest RAM 窗口 —— 按需分页的账本是
 * 「每 2 MiB 块一张页表」（aarch64 是 L3，riscv 是 L0），窗口比它大就会在
 * 缺页路径上被 ram_blk_index() 判为"窗口外"，表现是 guest 访问那段内存
 * 时被当成 MMIO 去解码，或者直接停机。两条常量分别定义在两个架构头里，
 * 只有这里同时看得见它们。
 */
#if ARCH_AARCH64
_Static_assert(GUEST_LINUX_MEM_SIZE / S2_BLOCK_SIZE <= S2_MAX_L3_TABLES,
               "GUEST_LINUX_MEM_SIZE 超出 S2_MAX_L3_TABLES 覆盖范围");
#elif ARCH_RISCV64
_Static_assert(GUEST_LINUX_MEM_SIZE / GSTAGE_BLOCK_SIZE <= GSTAGE_MAX_L0_TABLES,
               "GUEST_LINUX_MEM_SIZE 超出 GSTAGE_MAX_L0_TABLES 覆盖范围");
#elif ARCH_X86_64
_Static_assert(GUEST_LINUX_MEM_SIZE / EPT_BLOCK_SIZE <= EPT_MAX_PT_TABLES,
               "GUEST_LINUX_MEM_SIZE 超出 EPT_MAX_PT_TABLES 覆盖范围");
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

/* ── GPA → 宿主可写地址（按需分页下**唯一**的入口）────────────────
 *
 * ⚠️ 从前这里到处是 `phys_to_virt(gpa)` —— 因为 guest RAM 是 identity 映射
 * （GPA == PA），随手一算就能写。按需分页之后这两条前提都没了：
 *   - 同一个 GPA 在不同 VM 里指向**不同的**物理页（隔离就靠这个）；
 *   - 页面可能**还没分配**（首次访问才由缺页处理补上）。
 * 所以宿主代码要碰 guest 内存，一律走这里。
 *
 * @alloc: 允许在未映射时现分配一页（加载映像时为 1；只想看看时为 0）。
 * 返回 NULL 表示"没映射且不允许分配"或"PMN 没页了"。
 */
void *guest_loader_gpa_ptr(vm_t *vm, uint64_t gpa, int alloc) {
  /*
   * ⚠️ 两条路径都必须把**页内偏移**加回去。
   *
   * map_* 系列返回的是**页基址**（它们只负责把 gpa 所在的页映射好），而
   * lookup 系列返回的地址里已经带了偏移（`(entry & ~0xFFF) | (gpa & 0xFFF)`）。
   * 早先 map 那条路忘了加，症状极具误导性：
   *   - **顺序装载、且起点页对齐**时完全正常 —— 一页里第一次写 off==0，
   *     之后同一页的写都走 lookup 分支（已经映射了），偏移是对的；
   *   - 而**起点不在页边界**的一次性写入（比如 x86 的 MP 表，GPA 0x9F800）
   *     会把整段数据写到**页首**去，目标位置留下一片零，且没有任何报错。
   * aarch64/riscv 的 guest 镜像恰好都是页对齐顺序装载，所以一直没暴露。
   */
#if ARCH_AARCH64
  uint64_t pa = 0;

  if (stage2_lookup(&vm->s2, gpa, &pa))
    return phys_to_virt(pa);
  if (!alloc)
    return NULL;

  pa = stage2_map_page(&vm->s2, gpa, 1 /*zero*/);
  if (!pa)
    return NULL;
  vm->s2.nr_premap++;   /* 加载期分配的页（与缺页驱动的 nr_fault 区分统计）*/
  return phys_to_virt(pa | (gpa & 0xFFF));
#elif ARCH_RISCV64
  uint64_t pa = 0;

  if (rv_gstage_lookup(&vm->gstage, gpa, &pa))
    return phys_to_virt(pa);
  if (!alloc)
    return NULL;

  pa = rv_gstage_map_page(&vm->gstage, gpa, 1 /*zero*/);
  if (!pa)
    return NULL;
  vm->gstage.nr_premap++;   /* 加载期分配的页（与缺页驱动的 nr_fault 区分统计）*/
  return phys_to_virt(pa | (gpa & 0xFFF));
#elif ARCH_X86_64
  uint64_t hpa = 0;

  if (x86_ept_lookup(&vm->ept, gpa, &hpa))
    return phys_to_virt(hpa);
  if (!alloc)
    return NULL;

  hpa = x86_ept_map_page(&vm->ept, gpa, 1 /*zero*/);
  if (!hpa)
    return NULL;
  vm->ept.nr_premap++;      /* 加载期分配的页（与缺页驱动的 nr_fault 区分统计）*/
  return phys_to_virt(hpa | (gpa & 0xFFF));
#else
  /* 还没有 stage-2 的架构（vmm_test 的玩具 guest）：identity 映射 */
  (void)vm;
  (void)alloc;
  return phys_to_virt(gpa);
#endif
}

/*
 * ── 页感知的 guest 内存访问族 ───────────────────────────────
 *
 * ⚠️ **不要对 guest_loader_gpa_ptr() 的返回值做跨页的指针算术。**
 *
 * identity 映射时代 `phys_to_virt(gpa)` 是线性的，于是 `p + off`（off 超过
 * 一页）恰好就是"gpa + off 那一页"—— 很多代码靠这个跨页 memcpy/memmove。
 * 改成按需分页之后那条不变量没了：每个 guest 页都是各自从 PMM 分配的、
 * **物理上不连续**，`p + off` 走到的是宿主物理内存里的下一页，而不是 guest
 * 的下一页。表现是"数据搬过去了一部分，另一部分是宿主的随机内容"，且没有
 * 任何报错。
 *
 * x86 的 bzImage 装载就栽在这里：11 MB 的保护模式内核用一句 memmove 搬运，
 * 搬完 guest 的 GDT（在 payload 末尾 0xAA7B40）是垃圾，`lgdt` 之后
 * `mov %ax,%ds` 直接 #GP → IDT 还没建 → **triple fault**。
 *
 * 所以跨页的一律走下面这几个：它们逐页翻译、逐页搬。
 */
int guest_loader_write_guest(vm_t *vm, uint64_t gpa, const void *src, size_t n)
{
  const uint8_t *s = (const uint8_t *)src;

  while (n > 0) {
    uint64_t off_in_page = gpa & 0xFFFULL;
    size_t chunk = (size_t)(4096 - off_in_page);
    void *dst;

    if (chunk > n)
      chunk = n;

    dst = guest_loader_gpa_ptr(vm, gpa, 1);
    if (!dst) {
      KLOG_ERROR("[guest] write_guest: cannot map gpa=0x%llx\n",
                 (unsigned long long)gpa);
      return -1;
    }
    memcpy(dst, s, chunk);

    gpa += chunk;
    s += chunk;
    n -= chunk;
  }
  return 0;
}

/* 从 guest 内存读一段到宿主缓冲（逐页）。*/
int guest_loader_read_guest(vm_t *vm, uint64_t gpa, void *dst, size_t n)
{
  uint8_t *d = (uint8_t *)dst;

  while (n > 0) {
    uint64_t off_in_page = gpa & 0xFFFULL;
    size_t chunk = (size_t)(4096 - off_in_page);
    const void *src;

    if (chunk > n)
      chunk = n;

    src = guest_loader_gpa_ptr(vm, gpa, 0 /*只读，不分配*/);
    if (!src) {
      KLOG_ERROR("[guest] read_guest: gpa=0x%llx unmapped\n",
                 (unsigned long long)gpa);
      return -1;
    }
    memcpy(d, src, chunk);

    gpa += chunk;
    d += chunk;
    n -= chunk;
  }
  return 0;
}

/* 往 guest 内存填一段字节（逐页）。*/
int guest_loader_fill_guest(vm_t *vm, uint64_t gpa, int byte, size_t n)
{
  while (n > 0) {
    uint64_t off_in_page = gpa & 0xFFFULL;
    size_t chunk = (size_t)(4096 - off_in_page);
    void *dst;

    if (chunk > n)
      chunk = n;

    dst = guest_loader_gpa_ptr(vm, gpa, 1);
    if (!dst) {
      KLOG_ERROR("[guest] fill_guest: cannot map gpa=0x%llx\n",
                 (unsigned long long)gpa);
      return -1;
    }
    memset(dst, byte, chunk);

    gpa += chunk;
    n -= chunk;
  }
  return 0;
}

/* ── 文件加载 ─────────────────────────────────────────────── */

/*
 * guest_loader_read_file_range — 把文件的 [file_off, file_off+len) 读进宿主缓冲
 *
 * 与 guest_loader_read_file() 的区别：能从中间读。bzImage 要用它先取头部
 * （解析 setup_sects / pref_address），再决定各段装到哪。
 * len == 0 表示读到文件尾。返回读到的字节数（可能少于 len），失败返回 -1。
 */
int guest_loader_read_file_range(const char *path, uint64_t file_off,
                                 void *buf, size_t len) {
  vfs_file_t *f = NULL;
  uint8_t *d = (uint8_t *)buf;
  size_t total = 0;
  int rc;

  if (file_off > 0) {
    uint64_t new_off = 0;
    rc = vfs_open(path, 0, 0, &f);
    if (rc != 0 || !f)
      return -1;
    rc = vfs_seek(f, (int64_t)file_off, 0 /*SEEK_SET*/, &new_off);
    if (rc != 0) {
      vfs_close(f);
      return -1;
    }
  } else {
    rc = vfs_open(path, 0, 0, &f);
    if (rc != 0 || !f)
      return -1;
  }

  while (len == 0 || total < len) {
    size_t want = (len == 0) ? 4096 : (len - total);
    int n;

    if (want > 4096)
      want = 4096;
    n = vfs_read_to_phys(f, file_off + total, d + total, want);
    if (n <= 0)
      break;
    total += (size_t)n;
    if ((size_t)n < want && len != 0)
      break;                      /* 文件尾 */
  }

  vfs_close(f);
  return (int)total;
}

/*
 * guest_loader_file_size — 取文件大小（字节），失败返回 -1
 *
 * ⚠️ 不要用 bzImage 头里的 `syssize`：那是 16 位时代以 16 字节为单位的旧账，
 * 对现代 bzImage 不准。seek 到末尾最可靠。
 */
int guest_loader_file_size(const char *path) {
  vfs_file_t *f = NULL;
  uint64_t end = 0;
  int rc;

  rc = vfs_open(path, 0, 0, &f);
  if (rc != 0 || !f)
    return -1;
  rc = vfs_seek(f, 0, 2 /*SEEK_END*/, &end);
  vfs_close(f);
  if (rc != 0)
    return -1;
  return (int)end;
}

/*
 * guest_loader_load_range — 把文件的 [file_off, file_off+len) 直接装到 guest 的 gpa
 *
 * len == 0 表示读到文件尾。
 *
 * ⚠️ 这是 x86 bzImage 装载的**首选**方式，别再用"整个文件装到暂存区再搬"那套。
 * 原因有二：
 *   1. 那段搬运在按需分页下必须走逐页的 guest→guest 拷贝（见
 *      guest_loader_write_guest 的说明），凭空多一层容易出错的机制；
 *   2. 直接按最终地址装载，连 11 MB 的临时副本都不用，启动更快。
 * 两条装载区间（setup 与保护模式内核）本来就是分开的，天然没有重叠。
 *
 * 返回装上的字节数，失败返回 -1。
 */
int guest_loader_load_range(vm_t *vm, const char *path, uint64_t file_off,
                            uint64_t gpa, uint64_t len) {
  vfs_file_t *f = NULL;
  uint64_t off = file_off;
  uint64_t done = 0;
  int rc = vfs_open(path, 0 /*O_RDONLY*/, 0, &f);

  if (rc != 0 || !f) {
    KLOG_ERROR("[guest] cannot open '%s' (rc=%d)\n", path, rc);
    return -1;
  }
  if (file_off > 0) {
    uint64_t new_off = 0;
    if (vfs_seek(f, (int64_t)file_off, 0, &new_off) != 0) {
      vfs_close(f);
      return -1;
    }
  }

  while (len == 0 || done < len) {
    size_t want = (len == 0) ? sizeof(g_load_buf)
                             : (size_t)(len - done);
    int n;

    if (want > sizeof(g_load_buf))
      want = sizeof(g_load_buf);

    n = vfs_read_to_phys(f, off, g_load_buf, want);
    if (n <= 0)
      break;

    if (guest_loader_write_guest(vm, gpa + done, g_load_buf, (size_t)n) != 0) {
      vfs_close(f);
      return -1;
    }

    off += (uint64_t)n;
    done += (uint64_t)n;

    if ((size_t)n < want && len != 0)
      break;                      /* 文件尾 */
  }

  vfs_close(f);
  return (int)done;
}

int guest_loader_load_file(vm_t *vm, const char *path, uint64_t gpa) {
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

    /* 写进 guest 内存：按需分页下必须经 guest_loader_write_guest（见它的注释）*/
    if (guest_loader_write_guest(vm, gpa + off, g_load_buf, (size_t)n) != 0) {
      total = -1;
      break;
    }

    off += (uint64_t)n;
    total += n;

    if (n < (int)sizeof(g_load_buf))
      break; /* 读到文件尾 */
  }

  vfs_close(f);
  KLOG_INFO("[guest] loaded '%s': %d bytes -> GPA 0x%llx\n", path, total,
            (unsigned long long)gpa);
  return total;
}

/*
 * 把文件读进**宿主缓冲**（不碰 guest 内存）。
 *
 * DTB 补丁必须走这条路：补丁代码是按线性偏移索引 FDT 的，而在按需分页下
 * 一个 4 KB 的 DTB 可能落在**不连续的**物理页上 —— 直接对 guest 内存做
 * `dtb[i]` 会写坏宿主或别的 VM。所以：读到宿主缓冲 → 在缓冲上打补丁 →
 * 一次 guest_loader_write_guest() 写回去。
 *
 * 返回读到的字节数；缓冲不够放下整个文件时返回 -1（宁可不启动也不能截断
 * DTB —— 截断的 FDT 会让 guest 解析到垃圾）。
 */
static int guest_loader_read_file(const char *path, uint8_t *buf, size_t cap) {
  int n = guest_loader_read_file_range(path, 0, buf, cap);

  if (n < 0)
    return -1;

  /* 缓冲满了但文件可能还没读完 —— 再探一个字节 */
  if ((size_t)n == cap) {
    uint8_t probe;
    if (guest_loader_read_file_range(path, (uint64_t)cap, &probe, 1) > 0) {
      KLOG_ERROR("[guest] '%s' larger than %zu bytes buffer\n", path, cap);
      return -1;
    }
  }
  return n;
}

/* ── DTB 修补 ─────────────────────────────────────────────── */
/*
 * 遍历 FDT 结构块，把 initrd 的 start/end 两个 64 位属性改写到指定值。
 * （对标 kvmm loader.rs 的 patch_dtb_initrd）
 */
int guest_loader_patch_dtb_initrd(uint8_t *dtb, uint32_t dtb_size,
                                  uint64_t initrd_start, uint64_t initrd_end) {

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
int guest_loader_patch_dtb_memory(uint8_t *dtb, uint32_t dtb_size,
                                  uint64_t mem_base, uint64_t mem_size) {

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

int guest_loader_patch_dtb_bootargs(uint8_t *dtb, uint32_t dtb_size,
                                    const char *bootargs) {

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

int guest_loader_nop_dtb_nodes(uint8_t *dtb, uint32_t dtb_size,
                               const char *const *names, int nr_names) {

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
  " console=ttyAMA0 rdinit=/init  oops=panic"

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
  "quiet console=ttyS0 rdinit=/init panic_on_warn=0 oops=panic"

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
extern int x86_guest_boot(vm_t *vm);
#endif

int guest_loader_run_linux(vm_t *vm) {
#if ARCH_X86_64
  return x86_guest_boot(vm);
#else
  int rc;

  /*
   * 重入保护：调用者（/dev/vmm 或直启路径）已经从 VM 池里拿到一个槽位并
   * 置成 LOADING，这里只需确认它可用。（从前是一个全局 "guest 在跑吗"，
   * 那在多 VM 下会拦住第二个 VM。）
   */
  if (!vm || vm->state != VM_LOADING) {
    KLOG_WARN("[guest] run_linux: vm slot not in LOADING state, refusing\n");
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

  /*
   * ── 建立 VM（**必须**在加载映像之前）──────────────────────────
   *
   * stage-2 是在 vm_create() 里初始化的 —— 一张**空表**。而加载映像要往
   * guest 物理地址里写，那需要映射。按需分页下 guest_loader_write_guest() 会为每一页
   * 现分配，所以这里**不再需要**预先预留物理内存或整片清零：
   *
   *   pmm_mark_allocated(192 MiB)   ← 删掉了
   *   memset(192 MiB)               ← 删掉了
   *
   * 从前那两行既是"只能有一个 VM"的根源（第二份无处安放、memset 会抹掉
   * 第一个 VM 的内存），也让每个 VM 无论用多少都硬吃 192 MiB。现在宿主
   * 只为 guest 真正碰过的页付内存，且两个 VM 的页天然隔离。
   */
  rc = vm_create(vm);
  if (rc != 0) {
    KLOG_ERROR("[guest] vm_create failed\n");
    return -1;
  }

  /* 1. 加载 kernel Image */
  int klen = guest_loader_load_file(vm, GUEST_LINUX_KERNEL_PATH,
                                    GUEST_LINUX_KERNEL_GPA);
  if (klen <= 0)
    return -1;
  KLOG_INFO("[guest] kernel: %d bytes\n", klen);

  /*
   * 2. DTB：**读进宿主缓冲**再打补丁，最后一次性写回 guest。
   *
   * 不能像从前那样直接对 guest 内存 `dtb[i]`：补丁代码按线性偏移索引 FDT，
   * 而按需分页下 4 KB 的 DTB 可能落在不连续的物理页上 —— 越过页边界继续
   * 线性索引就会写到宿主或别的 VM 的内存里。
   */
  static uint8_t dtb_buf[8192];
  int dlen = guest_loader_read_file(GUEST_LINUX_DTB_PATH, dtb_buf,
                                    sizeof(dtb_buf));
  if (dlen <= 0)
    return -1;

  guest_loader_patch_dtb_memory(dtb_buf, (uint32_t)dlen,
                                GUEST_LINUX_MEM_BASE, GUEST_LINUX_MEM_SIZE);

  if (guest_loader_patch_dtb_bootargs(dtb_buf, (uint32_t)dlen,
                                      GUEST_LINUX_BOOTARGS) != 0) {
    KLOG_ERROR("[guest] bootargs 补丁失败：guest 会用 DTB 自带的那句命令行，"
               "GUEST_LINUX_BOOTARGS 不生效（原因见上一条 WARN）\n");
  }

  /* 3. 加载 initrd */
  int ilen = guest_loader_load_file(vm, GUEST_LINUX_INITRD_PATH,
                                    GUEST_LINUX_INITRD_GPA);
  if (ilen <= 0)
    return -1;

  guest_loader_patch_dtb_initrd(dtb_buf, (uint32_t)dlen,
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
        dtb_buf, (uint32_t)dlen, unsupported_nodes,
        (int)(sizeof(unsupported_nodes) / sizeof(unsupported_nodes[0])));
  }

  /* 补好的 DTB 一次性写进 guest（memory/bootargs/initrd 都改完了）*/
  if (guest_loader_write_guest(vm, GUEST_LINUX_DTB_GPA, dtb_buf, (size_t)dlen) != 0)
    return -1;

  /*
   * 5. 配置 vCPU 的入口状态
   *
   * 必须在 vm_create() **之后**：两个架构的 vm_init 都会 memset 整个
   * vcpu 数组，先写会被清掉。
   */
  vcpu_t *vcpu = &vm->vcpus[0];

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
