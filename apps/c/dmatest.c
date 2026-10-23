/*
   dmatest.c — dma-buf / dma-heap(P0)的用户态验收程序

   跑法：
       make PLATFORM=qemu-virt-aarch64 DMA=1 rootfs
       make PLATFORM=qemu-virt-aarch64 DMA=1 run-fs
       ~ # /bin/dmatest

   验收目标(对应 docs/npu/RK3588_NPU_DRIVER.md §5 的 P0)：
       open(/dev/dma_heap/...) → DMA_HEAP_IOCTL_ALLOC → mmap → 读写一致,
       并且反复分配/关闭不泄漏(fd 池和缓冲区表都只有 128 个槽,
       漏一个跑 128 轮就会现形)。

   注意： 本程序不碰 NPU, 所以它能证明的只有"管路通"：
      缓存维护那两条(DMA_BUF_IOCTL_SYNC)只会被执行, 不会产生可观测差异
      —— 同一个 CPU 自己写的自己读, cache 天然一致。真正的正确性要等
      P3 有设备 DMA 进来才验得了。这里只能确认"ioctl 返回 0, 不报错"。

   注意： 下面的结构体与 ioctl 号是手抄的(用户态不能包含内核头)。
      _IOC 把 sizeof 编进了操作码, 所以尺寸写错不是"参数错位"而是换成
      另一个 ioctl 号 —— 内核那边会走到 default 分支返回 -ENOSYS,
      是响亮的失败, 不是静默损坏。内核侧同名定义见 include/dma_buf.h。
*/
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

/* ── 与 include/dma_buf.h 逐字节一致 ───────────────────────────────────── */
struct dma_heap_allocation_data {
    uint64_t len;
    uint32_t fd;
    uint32_t fd_flags;
    uint64_t heap_flags;
};
#define DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0, struct dma_heap_allocation_data)

#define DMA_BUF_SYNC_READ  (1U << 0)
#define DMA_BUF_SYNC_WRITE (1U << 1)
#define DMA_BUF_SYNC_RW    (DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE)
#define DMA_BUF_SYNC_START (0U << 2)
#define DMA_BUF_SYNC_END   (1U << 2)

struct dma_buf_sync {
    uint64_t flags;
};
#define DMA_BUF_IOCTL_SYNC _IOW('b', 0, struct dma_buf_sync)

/* ── 计数 ──────────────────────────────────────────────────────────────── */
static int g_pass, g_fail;

static void ok(const char *what)
{
    printf("  [ok]   %s\n", what);
    g_pass++;
}

static void bad(const char *what, const char *why)
{
    printf("  [FAIL] %s — %s\n", what, why);
    g_fail++;
}

/* ── 一次完整的「分配 → 映射 → 读写 → 同步 → 关闭」 ───────────────────── */
static int roundtrip(int heap_fd, size_t len, uint8_t seed, const char *tag)
{
    struct dma_heap_allocation_data args;
    memset(&args, 0, sizeof(args));
    args.len = len;
    args.fd_flags = O_RDWR;

    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &args) < 0) {
        printf("  [FAIL] %s: alloc(%zu) ioctl 失败: %s\n", tag, len, strerror(errno));
        g_fail++;
        return -1;
    }
    if (args.fd == 0 || args.fd == (uint32_t)-1) {
        printf("  [FAIL] %s: alloc 返回的 fd 不合理 (%d)\n", tag, (int)args.fd);
        g_fail++;
        return -1;
    }

    int bfd = (int)args.fd;
    uint8_t *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, bfd, 0);
    if (p == MAP_FAILED) {
        printf("  [FAIL] %s: mmap(len=%zu) 失败: %s\n", tag, len, strerror(errno));
        g_fail++;
        close(bfd);
        return -1;
    }

    /* 写一个可校验的花纹(seed 让不同缓冲区互不相同) */
    for (size_t i = 0; i < len; i++)
        p[i] = (uint8_t)(seed + i * 7U);

    /* CPU 写完了要交给"设备"看 → END|WRITE(clean) */
    struct dma_buf_sync s;
    s.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
    if (ioctl(bfd, DMA_BUF_IOCTL_SYNC, &s) < 0) {
        printf("  [FAIL] %s: SYNC(END|WRITE) 失败: %s\n", tag, strerror(errno));
        g_fail++;
    }

    /* "设备"写完了 CPU 要读 → START|READ(invalidate), 再读回来校验 */
    s.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
    if (ioctl(bfd, DMA_BUF_IOCTL_SYNC, &s) < 0) {
        printf("  [FAIL] %s: SYNC(START|READ) 失败: %s\n", tag, strerror(errno));
        g_fail++;
    }

    size_t bad_at = len;
    for (size_t i = 0; i < len; i++) {
        if (p[i] != (uint8_t)(seed + i * 7U)) {
            bad_at = i;
            break;
        }
    }
    if (bad_at != len) {
        printf("  [FAIL] %s: 读回校验不一致 @%zu (期望 0x%02x 实得 0x%02x)\n", tag, bad_at,
               (uint8_t)(seed + bad_at * 7U), p[bad_at]);
        g_fail++;
    }

    munmap(p, len);
    close(bfd); /* 关闭应当把缓冲区还给堆 */
    return 0;
}

int main(void)
{
    printf("=== dmatest: dma-buf / dma-heap (P0) ===\n");

    /* ── 1. 打开堆 ─────────────────────────────────────────────────── */
    const char *names[] = { "/dev/dma_heap/cma", "/dev/dma_heap/system" };
    int heap_fd = -1;
    const char *heap_name = NULL;
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        heap_fd = open(names[i], O_RDWR);
        if (heap_fd >= 0) {
            heap_name = names[i];
            break;
        }
    }
    if (heap_fd < 0) {
        printf("[FAIL] 打不开任何 dma-heap (试过 %s / %s): %s\n", names[0], names[1], strerror(errno));
        return 1;
    }
    printf("堆: %s (fd=%d)\n", heap_name, heap_fd);
    ok("open(dma_heap)");

    /* ── 2. 各种尺寸的往返 ─────────────────────────────────────────── */
    printf("\n-- 不同尺寸 --\n");
    roundtrip(heap_fd, 4096, 0x11, "4 KiB");
    roundtrip(heap_fd, 100, 0x22, "100 B（非页对齐）");
    roundtrip(heap_fd, 1 << 20, 0x33, "1 MiB");

    /* ── 3. 多个缓冲区必须互不重叠 ─────────────────────────────────── */
    printf("\n-- 并发多缓冲区 --\n");
    {
        enum {
            N = 8
        };
        int fds[N];
        uint8_t *maps[N];
        const size_t L = 8192;
        int alloced = 0;

        for (int i = 0; i < N; i++) {
            struct dma_heap_allocation_data a;
            memset(&a, 0, sizeof(a));
            a.len = L;
            if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &a) < 0)
                break;
            fds[i] = (int)a.fd;
            maps[i] = mmap(NULL, L, PROT_READ | PROT_WRITE, MAP_SHARED, fds[i], 0);
            if (maps[i] == MAP_FAILED) {
                close(fds[i]);
                break;
            }
            memset(maps[i], 0x40 + i, L);
            alloced++;
        }

        if (alloced != N) {
            bad("同时持有 8 个缓冲区", "分配失败");
        } else {
            int overlap = 0;
            for (int i = 0; i < N; i++) {
                for (size_t k = 0; k < L; k++) {
                    if (maps[i][k] != (uint8_t)(0x40 + i)) {
                        overlap = 1;
                        break;
                    }
                }
                if (overlap)
                    break;
            }
            if (overlap)
                bad("8 个缓冲区内容互不干扰", "有缓冲区被别人的数据覆盖");
            else
                ok("8 个缓冲区各自独立、互不覆盖");
        }
        for (int i = 0; i < alloced; i++) {
            munmap(maps[i], L);
            close(fds[i]);
        }
    }

    /* ── 4. 反复分配/关闭不泄漏 ─────────────────────────────────────── */
    printf("\n-- 泄漏检查（300 轮）--\n");
    {
        /*
           fd 池和 dma_buf 表都只有 128 个槽。若 close 路径漏还引用
           (fd_obj_close 没走到 dma_buf_put_index), 这里 128 轮内必然失败。
        */
        int fail_at = -1;
        for (int i = 0; i < 300; i++) {
            if (roundtrip(heap_fd, 4096, (uint8_t)i, "") != 0) {
                fail_at = i;
                break;
            }
        }
        if (fail_at >= 0)
            bad("300 轮分配/释放", "中途失败（见上）—— 疑似泄漏");
        else
            ok("300 轮分配/释放无泄漏");
    }

    /* ── 5. 越界 mmap 必须被拒 ─────────────────────────────────────── */
    printf("\n-- 边界 --\n");
    {
        struct dma_heap_allocation_data a;
        memset(&a, 0, sizeof(a));
        a.len = 4096;
        if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &a) == 0) {
            int bfd = (int)a.fd;
            void *p = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, bfd, 0);
            if (p == MAP_FAILED)
                ok("超出缓冲区大小的 mmap 被拒绝");
            else {
                bad("超出缓冲区大小的 mmap 被拒绝", "竟然映射成功了");
                munmap(p, 8192);
            }
            close(bfd);
        } else {
            bad("越界 mmap 用例准备", "alloc 失败");
        }
    }

    /* ── 6. fork 之后仍然是同一块物理内存 ─────────────────────────── */
    printf("\n-- fork 共享语义 --\n");
    {
        struct dma_heap_allocation_data a;
        memset(&a, 0, sizeof(a));
        a.len = 4096;
        if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &a) != 0) {
            bad("fork 用例准备", "alloc 失败");
        } else {
            int bfd = (int)a.fd;
            uint32_t *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, bfd, 0);
            if (p == MAP_FAILED) {
                bad("fork 用例准备", "mmap 失败");
                close(bfd);
            } else {
                /*
                   打的是 PTE_KEEP 的 clone 分支：认不出这一位的实现会把设备
                   缓冲当成普通私有页复制一份给子进程 —— 内存是安全的,
                   但父子从此看着两块不同的内存。这种错不崩, 只让结果对不上。
                */
                p[0] = 0xAAAA0001U;
                pid_t pid = fork();
                if (pid == 0) {
                    /* 子进程：必须看到父进程写的值(同一块内存, 不是副本) */
                    if (p[0] != 0xAAAA0001U)
                        _exit(2);
                    p[0] = 0xBBBB0002U;
                    _exit(0);
                }
                int st = 0;
                if (pid > 0)
                    waitpid(pid, &st, 0);

                if (pid <= 0)
                    bad("fork", "fork 失败");
                else if (!WIFEXITED(st))
                    bad("子进程正常退出", "被信号杀死");
                else if (WEXITSTATUS(st) == 2)
                    bad("子进程看到父进程的写入", "读到的是副本 —— 设备缓冲被当成私有页复制了");
                else if (WEXITSTATUS(st) != 0)
                    bad("子进程正常退出", "退出码非 0");
                else if (p[0] != 0xBBBB0002U)
                    bad("父进程看到子进程的写入", "父子各拿到一份副本");
                else
                    ok("fork 后父子仍是同一块缓冲");

                munmap(p, 4096);
                close(bfd);
            }
        }
    }

    close(heap_fd);

    printf("\n=== 结果: %d 通过, %d 失败 ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
