/*
 * kernel/vmm/image_load.c — 从 rootfs 往 guest 内存装载文件/区间
 *
 * 从 guest_loader.c 拆出来的。它是「VFS 读 → 逐块写 guest 内存」这条通路，
 * 与 DTB 补丁无关；DTB 那部分在 fdt.c。
 *
 * 静态加载缓冲（LOAD_CHUNK / g_load_buf）只被这一组使用，所以跟着搬过来 ——
 * 它本来也不该由 share 整个文件的那个 TU 持有。
 *
 * 原型都在 include/guest_loader.h（公共头）。
 */

#include "guest_loader.h"
#include "klog.h"
#include "vfs.h" /* vfs_open / vfs_read_to_phys / vfs_seek / vfs_close */
#include "vmm/vmm.h"

/* 加载缓冲（静态，避免大栈占用）*/
#define LOAD_CHUNK 4096
static uint8_t g_load_buf[LOAD_CHUNK];

/* ── 文件加载 ─────────────────────────────────────────────── */

/*
 * guest_loader_read_file_range — 把文件的 [file_off, file_off+len) 读进宿主缓冲
 *
 * 与 guest_loader_read_file() 的区别：能从中间读。bzImage 要用它先取头部
 * （解析 setup_sects / pref_address），再决定各段装到哪。
 * len == 0 表示读到文件尾。返回读到的字节数（可能少于 len），失败返回 -1。
 */
int guest_loader_read_file_range(const char *path, uint64_t file_off, void *buf,
                                 size_t len)
{
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
            break; /* 文件尾 */
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
int guest_loader_file_size(const char *path)
{
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
                            uint64_t gpa, uint64_t len)
{
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
        size_t want = (len == 0) ? sizeof(g_load_buf) : (size_t)(len - done);
        int n;

        if (want > sizeof(g_load_buf))
            want = sizeof(g_load_buf);

        n = vfs_read_to_phys(f, off, g_load_buf, want);
        if (n <= 0)
            break;

        if (guest_loader_write_guest(vm, gpa + done, g_load_buf, (size_t)n) !=
            0) {
            vfs_close(f);
            return -1;
        }

        off += (uint64_t)n;
        done += (uint64_t)n;

        if ((size_t)n < want && len != 0)
            break; /* 文件尾 */
    }

    vfs_close(f);
    return (int)done;
}

int guest_loader_load_file(vm_t *vm, const char *path, uint64_t gpa)
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

        /* 写进 guest 内存：按需分页下必须经 guest_loader_write_guest（见它的注释）*/
        if (guest_loader_write_guest(vm, gpa + off, g_load_buf, (size_t)n) !=
            0) {
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
int guest_loader_read_file(const char *path, uint8_t *buf, size_t cap)
{
    int n = guest_loader_read_file_range(path, 0, buf, cap);

    if (n < 0)
        return -1;

    /* 缓冲满了但文件可能还没读完 —— 再探一个字节 */
    if ((size_t)n == cap) {
        uint8_t probe;
        if (guest_loader_read_file_range(path, (uint64_t)cap, &probe, 1) > 0) {
            KLOG_ERROR("[guest] '%s' larger than %zu bytes buffer\n", path,
                       cap);
            return -1;
        }
    }
    return n;
}
