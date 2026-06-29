/*
 * kernel/loader/elf_loader.c — ELF 程序加载器（文件 I/O 层）
 *
 * 职责：从 ext4 文件系统读取 ELF 文件，然后交由 task_execve 处理。
 * ELF 格式解析、段加载和进程创建逻辑分别在 elf_image.c 和 task/exec.c 中。
 */

#include "loader/bin_loader.h"
#include "elf.h"
#include "klog.h"
#include "kmalloc.h"
#include "mm_vm.h"
#include "string.h"
#include "task/exec.h"
#include <ext4.h>
#include <ext4_types.h>

#define MAX_FILE_SIZE     (10 * 1024 * 1024) /* 10MB */

/**
 * resolve_symlink - 跟随符号链接，最多 8 层，将最终真实路径写入 out。
 * 若路径不是符号链接，直接把 path 复制到 out 返回 0。
 * 支持相对符号链接（相对于当前目录）和绝对符号链接。
 * 返回 0 表示成功，-1 表示循环太深。
 */
static int resolve_symlink(const char *path, char *out, size_t outsz)
{
    char cur[256];
    int n = 0;
    while (path[n] && n < (int)sizeof(cur) - 1) { cur[n] = path[n]; n++; }
    cur[n] = '\0';

    for (int depth = 0; depth < 8; depth++) {
        char target[256];
        size_t rcnt = 0;
        int rc = ext4_readlink(cur, target, sizeof(target) - 1, &rcnt);
        if (rc != EOK) {
            /* 不是符号链接（或 readlink 失败）：cur 就是最终路径 */
            n = 0;
            while (cur[n] && n < (int)outsz - 1) { out[n] = cur[n]; n++; }
            out[n] = '\0';
            return 0;
        }
        target[rcnt] = '\0';

        /* 组合新路径 */
        char next[256];
        if (target[0] == '/') {
            /* 绝对符号链接 */
            n = 0;
            while (target[n] && n < (int)sizeof(next) - 1) { next[n] = target[n]; n++; }
            next[n] = '\0';
        } else {
            /* 相对符号链接：取 cur 的目录部分拼上 target */
            int slash = -1;
            for (int i = 0; cur[i]; i++)
                if (cur[i] == '/') slash = i;
            n = 0;
            if (slash >= 0) {
                for (int i = 0; i <= slash && n < (int)sizeof(next) - 1; i++, n++)
                    next[n] = cur[i];
            }
            for (int i = 0; target[i] && n < (int)sizeof(next) - 1; i++, n++)
                next[n] = target[i];
            next[n] = '\0';
        }
        /* next → cur，进入下一轮 */
        n = 0;
        while (next[n] && n < (int)sizeof(cur) - 1) { cur[n] = next[n]; n++; }
        cur[n] = '\0';
    }
    /* 符号链接层数超限 */
    return -1;
}

/**
 * elf_loader_load_from_file - 从文件系统加载并执行 ELF 程序
 * @pathname: 程序路径
 * @argv:     参数数组（可为 NULL）
 * @envp:     环境变量数组（可为 NULL）
 *
 * 读取 ELF 文件到内存，委托 task_execve 完成后续处理。
 * 返回：失败时返回负值；成功时 task_execve 不返回。
 */
int elf_loader_load_from_file(const char *pathname, char **argv, char **envp)
{
    ext4_file file;
    int rc;
    size_t fsize, rcnt;
    uint8_t *file_data;
    uint64_t file_size;
    char path_buf[256];
    uint32_t page_count;

    /* 复制路径名 */
    uint64_t i = 0;
    while (pathname[i] && i < sizeof(path_buf) - 1) {
        path_buf[i] = pathname[i];
        i++;
    }
    path_buf[i] = '\0';

    KLOG_DEBUG("[elf_loader] Loading: %s\n", path_buf);

    /* 解析符号链接（busybox applet 等均为 symlink） */
    char resolved[256];
    if (resolve_symlink(path_buf, resolved, sizeof(resolved)) == 0) {
        if (resolved[0] != path_buf[0] ||
            memcmp(resolved, path_buf, sizeof(path_buf)) != 0)
            KLOG_DEBUG("[elf_loader] Resolved '%s' -> '%s'\n", path_buf, resolved);
    } else {
        KLOG_WARN("[elf_loader] Symlink depth exceeded for '%s'\n", path_buf);
        memcpy(resolved, path_buf, sizeof(resolved));
    }

    /* 打开文件 */
    rc = ext4_fopen(&file, resolved, "r");
    if (rc != EOK) {
        KLOG_WARN("[elf_loader] Failed to open '%s': %d\n", resolved, rc);
        return -rc; /* lwext4 errno == Linux errno (ENOENT=2, etc.) */
    }

    /* 获取文件大小 */
    ext4_fseek(&file, 0, SEEK_END);
    fsize = ext4_ftell(&file);
    ext4_fseek(&file, 0, SEEK_SET);

    if (fsize == 0 || fsize > MAX_FILE_SIZE) {
        KLOG_ERROR("[elf_loader] Invalid file size: %zu\n", fsize);
        ext4_fclose(&file);
        return -8; /* -ENOEXEC */
    }

    file_size = (uint64_t)fsize;

    /* 分配内存 */
    page_count = (file_size + 4095) / 4096;
    file_data = (uint8_t *)kalloc_pages(page_count);
    if (file_data == NULL) {
        KLOG_ERROR("[elf_loader] Memory allocation failed\n");
        ext4_fclose(&file);
        return -12; /* -ENOMEM */
    }

    KLOG_DEBUG("[elf_loader] file_data=0x%llx pages=%u\n",
               (uint64_t)file_data, page_count);

    /* 读取文件 */
    rc = ext4_fread(&file, file_data, file_size, &rcnt);
    if (rc != EOK || rcnt != file_size) {
        KLOG_ERROR("[elf_loader] Read failed: rc=%d\n", rc);
        kfree_pages(file_data, page_count);
        ext4_fclose(&file);
        return -5; /* -EIO */
    }

    ext4_fclose(&file);

    KLOG_DEBUG("[elf_loader] File loaded: %zu bytes\n", file_size);

    /* ── 检查 PT_INTERP（动态连接器路径）───────────────────────────── */
    uint8_t  *interp_data  = NULL;
    uint64_t  interp_size  = 0;
    uint32_t  interp_pages = 0;
    {
        if (file_size >= sizeof(elf64_ehdr_t)) {
            elf64_ehdr_t *ehdr = (elf64_ehdr_t *)file_data;
            elf64_phdr_t *phdr = (elf64_phdr_t *)(file_data + ehdr->e_phoff);
            for (uint16_t pi = 0; pi < ehdr->e_phnum; pi++) {
                if (phdr[pi].p_type == PT_INTERP) {
                    char ipath[128] = {0};
                    uint64_t plen = phdr[pi].p_filesz;
                    if (plen >= sizeof(ipath)) plen = sizeof(ipath) - 1;
                    memcpy(ipath, file_data + phdr[pi].p_offset, plen);
                    ipath[plen] = '\0';
                    KLOG_DEBUG("[elf_loader] PT_INTERP: %s\n", ipath);

                    /* interpreter 路径也可能是符号链接 */
                    char iresolved[256];
                    if (resolve_symlink(ipath, iresolved, sizeof(iresolved)) != 0)
                        memcpy(iresolved, ipath, sizeof(iresolved));

                    ext4_file ifile;
                    int irc = ext4_fopen(&ifile, iresolved, "r");
                    if (irc != EOK) {
                        KLOG_ERROR("[elf_loader] Cannot open interpreter '%s': %d\n",
                                   iresolved, irc);
                        break;
                    }
                    ext4_fseek(&ifile, 0, SEEK_END);
                    size_t isz = ext4_ftell(&ifile);
                    ext4_fseek(&ifile, 0, SEEK_SET);

                    if (isz > 0 && isz <= MAX_FILE_SIZE) {
                        interp_pages = (isz + 4095u) / 4096u;
                        interp_data  = (uint8_t *)kalloc_pages(interp_pages);
                        if (interp_data) {
                            size_t iread = 0;
                            irc = ext4_fread(&ifile, interp_data, isz, &iread);
                            if (irc == EOK && iread == isz) {
                                interp_size = (uint64_t)isz;
                                KLOG_DEBUG("[elf_loader] Interpreter loaded: %zu bytes\n",
                                           isz);
                            } else {
                                KLOG_ERROR("[elf_loader] Interpreter read failed rc=%d\n",
                                           irc);
                                kfree_pages(interp_data, interp_pages);
                                interp_data  = NULL;
                                interp_pages = 0;
                            }
                        } else {
                            KLOG_ERROR("[elf_loader] OOM for interpreter\n");
                        }
                    } else {
                        KLOG_WARN("[elf_loader] Interpreter size invalid: %zu\n", isz);
                    }
                    ext4_fclose(&ifile);
                    break;
                }
            }
        }
    }

    /* 加载并执行 ELF（成功时不返回） */
    rc = task_execve(path_buf, file_data, file_size,
                     interp_data, interp_size, argv, envp);

    if (interp_data)
        kfree_pages(interp_data, interp_pages);
    kfree_pages(file_data, page_count);
    return rc;
}
