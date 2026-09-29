/*
 * fs/path.c - 路径规范化、用户/内核字符串拷贝、stat 填充
 *
 * 由 kernel/syscall/syscall.c 拆出。
 */
#include "syscall/fs/path.h"
#include "syscall/fs/fd_pool.h"
#include "syscall/syscall_internal.h"
#include "kernel_stat.h"
#include "string.h"
#include "mm_vm.h" /* PAGE_SIZE / phys_to_virt / mm_vm_get_paddr */
#include "vfs.h"
#include <ext4.h>
#include <ext4_errno.h>

#define USER_PTR_LIMIT 0x80000000ULL

static bool user_range_ok(const void *ptr, uint64_t len)
{
    uintptr_t start = (uintptr_t)ptr;
    uintptr_t end;

    if (start == 0)
        return false;
    if (len == 0)
        return true;

    end = start + len - 1;
    if (end < start)
        return false;
    return end < USER_PTR_LIMIT;
}

/*
 * user_range_accessible - 用户地址能否安全解引用：范围合法 **且每一页都映射着**
 *
 * user_range_ok() 只管前半段。它挡得住 (void*)-1（回绕），
 * 挡不住"范围合法、但已经 munmap"的地址 —— 那种地址会让后面 copy_*_bytes
 * 里的裸 memcpy 踩空 → **内核态 #PF → platform_panic → 整机停机**。
 *
 * LTP writev02 就是这么打的：它 mmap 一段让内核能写，然后 munmap，
 * 再把那个地址放进 iovec，期望内核返回 EFAULT 而不是崩。
 * 实测现场（修复前）：
 *   CR2=0x50002000  Error bits: P=0 W=0 U=0   ← 内核态读不存在的页
 *   #0 copy_from_user_bytes  #1 write_handler  #2 writev_handler
 *
 * 代价：每页一次多级页表遍历。选择"查得到才算合法"而不是 Linux 那套异常表
 * fixup —— 后者要在三个架构各写一份带 fixup 表的汇编，改动面大得多。
 *
 * 已知局限：**不解决 SMP 下的 TOCTOU** —— 查完到这里 memcpy 之间，别的核
 * 仍可能把这页拆掉。要彻底解决只能上异常表。
 */
bool user_range_accessible(const void *ptr, uint64_t len)
{
    if (!user_range_ok(ptr, len))
        return false;
    if (len == 0)
        return true;

    task_t *t = task_current();
    if (!t || !t->pgd)
        return true; /* 非用户进程上下文：维持旧行为，只做范围检查 */

    uint64_t start = (uint64_t)ptr;
    uint64_t first = start & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t last = (start + len - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    void *pgd = phys_to_virt((uint64_t)t->pgd);

    for (uint64_t va = first;; va += PAGE_SIZE) {
        if (mm_vm_get_paddr(pgd, va) == 0)
            return false;
        if (va == last)
            break;
    }
    return true;
}

void resolve_path(const char *cwd, const char *path, char *out, int outlen)
{
    char tmp[128];

    if (path == NULL || path[0] == '\0') {
        out[0] = '/';
        out[1] = '\0';
        return;
    }

    if (path[0] == '/') {
        int i = 0;
        while (path[i] && i < (int)sizeof(tmp) - 1) {
            tmp[i] = path[i];
            i++;
        }
        tmp[i] = '\0';
    } else {
        int i = 0;
        while (cwd[i] && i < (int)sizeof(tmp) - 1) {
            tmp[i] = cwd[i];
            i++;
        }
        if (i > 0 && tmp[i - 1] != '/' && i < (int)sizeof(tmp) - 1)
            tmp[i++] = '/';
        int j = 0;
        while (path[j] && i < (int)sizeof(tmp) - 1) {
            tmp[i++] = path[j++];
        }
        tmp[i] = '\0';
    }

    /* 规范化：处理 //、.、.. */
    int seg_start[64];
    int seg_len[64];
    int seg_count = 0;
    int i = 0;

    while (tmp[i] != '\0') {
        while (tmp[i] == '/')
            i++;
        if (tmp[i] == '\0')
            break;

        int start = i;
        while (tmp[i] != '\0' && tmp[i] != '/')
            i++;
        int len = i - start;

        if (len == 1 && tmp[start] == '.')
            continue;
        if (len == 2 && tmp[start] == '.' && tmp[start + 1] == '.') {
            if (seg_count > 0)
                seg_count--;
            continue;
        }

        if (seg_count < (int)(sizeof(seg_start) / sizeof(seg_start[0]))) {
            seg_start[seg_count] = start;
            seg_len[seg_count] = len;
            seg_count++;
        }
    }

    if (outlen <= 0)
        return;

    int pos = 0;
    out[pos++] = '/';
    for (int s = 0; s < seg_count && pos < outlen - 1; s++) {
        for (int k = 0; k < seg_len[s] && pos < outlen - 1; k++) {
            out[pos++] = tmp[seg_start[s] + k];
        }
        if (s != seg_count - 1 && pos < outlen - 1)
            out[pos++] = '/';
    }
    out[pos] = '\0';
}

int resolve_path_at(task_t *task, int dirfd, const char *pathname,
                    char *abspath, int abspath_len)
{
    if (!pathname)
        return -ENOENT;

    if (pathname[0] == '/') {
        resolve_path(task->cwd, pathname, abspath, abspath_len);
        return 0;
    }

    if (dirfd == AT_FDCWD) {
        resolve_path(task->cwd, pathname, abspath, abspath_len);
        return 0;
    }

    fd_obj_t *base = task_get_fd(task, dirfd);
    if (!base || !vfs_file_is_dir(fd_obj_file(base)))
        return -EBADF;

    resolve_path(base->path, pathname, abspath, abspath_len);
    return 0;
}

void follow_symlinks(char *out, size_t outsz)
{
    char cur[128];
    int n = 0;
    while (out[n] && n < 127) {
        cur[n] = out[n];
        n++;
    }
    cur[n] = '\0';

    for (int depth = 0; depth < 8; depth++) {
        char target[128];
        size_t rcnt = 0;
        if (ext4_readlink(cur, target, sizeof(target) - 1, &rcnt) != EOK)
            break;
        target[rcnt] = '\0';

        if (target[0] == '/') {
            n = 0;
            while (target[n] && n < 127) {
                cur[n] = target[n];
                n++;
            }
            cur[n] = '\0';
        } else {
            int slash = 0;
            for (int i = 0; cur[i]; i++)
                if (cur[i] == '/')
                    slash = i;
            char parent[128];
            int k;
            for (k = 0; k <= slash && k < 126; k++)
                parent[k] = cur[k];
            parent[k] = '\0';
            resolve_path(parent, target, cur, sizeof(cur));
        }
    }

    n = 0;
    while (cur[n] && n < (int)outsz - 1) {
        out[n] = cur[n];
        n++;
    }
    out[n] = '\0';
}

int copy_string_from_user(const char *ustr, char *kbuf, int maxlen)
{
    if (!user_range_ok(ustr, (uint64_t)maxlen))
        return -1;

    /*
     * 映射检查**逐页做**，而不是对 [ustr, ustr+maxlen) 整段预检。
     *
     * 原因：maxlen 是内核缓冲的大小（调用方给的是 sizeof(kbuf)），通常远大于
     * 字符串本身。整段预检会误杀"字符串贴在页尾、后面还有半个页没映射"这种
     * 完全合法的入参 —— 那是把现有能跑的东西改崩。
     * 逐页检查则只在真正要读某个新页之前查那一页，语义是"读得到的才算数"。
     *
     * 进入新页时 i 正好是该页的第一字节（(ustr+i) % PAGE_SIZE == 0）。
     */
    int i = 0;
    while (i < maxlen - 1) {
        if ((((uint64_t)ustr + (uint64_t)i) & (PAGE_SIZE - 1)) == 0 &&
            !user_range_accessible(ustr + i, 1))
            return -1;

        kbuf[i] = ustr[i];
        if (ustr[i] == '\0')
            return i;
        i++;
    }
    kbuf[i] = '\0';
    return i;
}

int copy_string_to_user(const char *kstr, char *ubuf, int maxlen)
{
    if (!user_range_ok(ubuf, (uint64_t)maxlen))
        return -1;
    int i = 0;
    while (i < maxlen - 1 && kstr[i]) {
        ubuf[i] = kstr[i];
        i++;
    }
    ubuf[i] = '\0';
    return i;
}

int copy_from_user_bytes(const void *usrc, void *kdst, uint64_t len)
{
    if (!user_range_accessible(usrc, len))
        return -1;
    memcpy(kdst, usrc, len);
    return 0;
}

int copy_to_user_bytes(const void *ksrc, void *udst, uint64_t len)
{
    if (!user_range_accessible(udst, len))
        return -1;
    memcpy(udst, ksrc, len);
    return 0;
}

int fill_stat_from_ext4(struct kernel_stat *st, const char *path)
{
    memset(st, 0, sizeof(*st));
    st->st_dev = 1;
    st->st_nlink = 1;
    st->st_blksize = 4096;

    uint32_t mode = 0;
    int rc = ext4_mode_get(path, &mode);
    if (rc != EOK)
        return -rc;
    st->st_mode = mode;

    uint32_t ino = 0;
    struct ext4_inode raw;
    if (ext4_raw_inode_fill(path, &ino, &raw) == EOK)
        st->st_ino = ino;

    if ((mode & 0170000) == 0100000) {
        /* 普通文件：size 要算上 i_size_hi，交给 lwext4 的 ext4_fsize */
        ext4_file f;
        rc = ext4_fopen2(&f, path, 0 /* O_RDONLY */);
        if (rc != EOK)
            return -rc;
        st->st_size = (int64_t)ext4_fsize(&f);
        st->st_blocks = (st->st_size + 511) / 512;
        ext4_fclose(&f);
    } else if ((mode & 0170000) == 0120000) {
        /*
         * 符号链接：size = 目标路径的字节数，正好是 readlink 会写出的长度。
         *
         * 原先是漏掉的（只处理了 S_IFREG），于是符号链接的 st_size 一直是
         * memset 之后的 0 —— 直接症状是 `ls -l` 的 size 列对符号链接显示 0
         * （应为目标串长度），另外 coreutils 会拿 st_size 当 readlink 缓冲区
         * 的初值，为 0 时会先按 1 字节读、再扩容重试。
         *
         * 这里不复用上面 ext4_raw_inode_fill 拿到的 raw inode：它给的是
         * **未转序**的磁盘原始结构（endian 要自己 to_le32），直接读字段会踩坑。
         * 走 ext4_readlink 量一次是公开 API、语义明确，代价是 stat 符号链接时
         * 多一次 open+read —— 不在热路径上。
         */
        char tgt[256];
        size_t rcnt = 0;
        if (ext4_readlink(path, tgt, sizeof(tgt), &rcnt) == EOK)
            st->st_size = (int64_t)rcnt;
    }

    return 0;
}
