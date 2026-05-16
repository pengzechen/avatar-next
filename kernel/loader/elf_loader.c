/*
 * kernel/loader/elf_loader.c — ELF 程序加载器（文件 I/O 层）
 *
 * 职责：从 ext4 文件系统读取 ELF 文件，然后交由 task_execve 处理。
 * ELF 格式解析、段加载和进程创建逻辑分别在 elf_image.c 和 task/exec.c 中。
 */

#include "loader/bin_loader.h"
#include "klog.h"
#include "pmm.h"
#include "mm_vm.h"
#include "string.h"
#include "task/exec.h"
#include <ext4.h>
#include <ext4_types.h>

extern pmm_t *g_pmm;

#define MAX_FILE_SIZE     (10 * 1024 * 1024) /* 10MB */

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

    KLOG_INFO("[elf_loader] Loading: %s\n", path_buf);

    /* 打开文件 */
    rc = ext4_fopen(&file, path_buf, "r");
    if (rc != EOK) {
        KLOG_ERROR("[elf_loader] Failed to open '%s': %d\n", path_buf, rc);
        return -1;
    }

    /* 获取文件大小 */
    ext4_fseek(&file, 0, SEEK_END);
    fsize = ext4_ftell(&file);
    ext4_fseek(&file, 0, SEEK_SET);

    if (fsize == 0 || fsize > MAX_FILE_SIZE) {
        KLOG_ERROR("[elf_loader] Invalid file size: %zu\n", fsize);
        ext4_fclose(&file);
        return -2;
    }

    file_size = (uint64_t)fsize;

    /* 分配内存 */
    page_count = (file_size + 4095) / 4096;
    uint64_t file_phys = pmm_alloc_pages(g_pmm, page_count);
    if (file_phys == 0) {
        KLOG_ERROR("[elf_loader] Memory allocation failed\n");
        ext4_fclose(&file);
        return -3;
    }

    file_data = (uint8_t *)phys_to_virt(file_phys);

    KLOG_INFO("[elf_loader] file_phys=0x%llx file_data=0x%llx pages=%u\n",
              file_phys, (uint64_t)file_data, page_count);

    /* 读取文件 */
    rc = ext4_fread(&file, file_data, file_size, &rcnt);
    if (rc != EOK || rcnt != file_size) {
        KLOG_ERROR("[elf_loader] Read failed: rc=%d\n", rc);
        pmm_free_pages(g_pmm, file_phys, page_count);
        ext4_fclose(&file);
        return -4;
    }

    ext4_fclose(&file);

    KLOG_INFO("[elf_loader] File loaded: %zu bytes\n", file_size);

    /* 加载并执行 ELF（成功时不返回） */
    rc = task_execve(path_buf, file_data, file_size, argv, envp);

    pmm_free_pages(g_pmm, file_phys, page_count);
    return rc;
}
