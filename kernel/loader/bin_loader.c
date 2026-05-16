/*
 * kernel/loader/bin_loader.c - 二进制程序加载器实现
 */

#include "loader/bin_loader.h"
#include "syscall/syscall.h"
#include "klog.h"
#include "task/task.h"
#include "pmm.h"
#include "mm_vm.h"
#include "user_layout.h"
#include <ext4.h>
#include <ext4_types.h>


#define MAX_FILE_SIZE        (1024 * 1024) /* 最大程序大小 1MB */

/**
 * bin_loader_load_from_file - 从文件系统加载并执行程序
 */
int bin_loader_load_from_file(const char *pathname, char **argv, char **envp)
{
    ext4_file file;
    int rc;
    size_t fsize, rcnt;
    uint8_t *code_buffer;
    uint64_t file_size;
    task_t *new_task;
    char path_buf[256];  /* 内核缓冲区，用于存储路径名 */

    (void)argv;  /* 暂未使用参数 */
    (void)envp;  /* 暂未使用环境变量 */

    /* 从用户空间复制路径名到内核缓冲区 */
    /* 简单实现：假设当前使用共享页表，可以直接访问 */
    /* TODO: 实现真正的 copy_from_user */
    uint64_t i = 0;
    while (pathname[i] && i < sizeof(path_buf) - 1) {
        path_buf[i] = pathname[i];
        i++;
    }
    path_buf[i] = '\0';

    KLOG_INFO("[loader] Loading program: %s\n", path_buf);

    /* 打开文件 */
    rc = ext4_fopen(&file, path_buf, "r");
    if (rc != EOK) {
        KLOG_ERROR("[loader] Failed to open '%s': %d\n", path_buf, rc);
        return -1;
    }

    /* 获取文件大小 */
    fsize = 0;
    ext4_fseek(&file, 0, SEEK_END);
    fsize = ext4_ftell(&file);
    ext4_fseek(&file, 0, SEEK_SET);

    KLOG_INFO("[loader] File size: %zu bytes\n", fsize);

    /* 检查文件大小 */
    if (fsize == 0 || fsize > MAX_FILE_SIZE) {
        KLOG_ERROR("[loader] Invalid file size: %zu\n", fsize);
        ext4_fclose(&file);
        return -2;
    }

    file_size = (uint64_t)fsize;

    /* 计算需要的页面数 */
    uint32_t page_count = (file_size + 4095) / 4096;

    /* 分配内存页面 */
    uint64_t code_phys = pmm_alloc_pages(g_pmm, page_count);
    if (code_phys == 0) {
        KLOG_ERROR("[loader] Failed to allocate memory for program\n");
        ext4_fclose(&file);
        return -3;
    }

    /* pmm_alloc_pages 返回物理地址，需转换为内核虚拟地址才能访问 */
    code_buffer = (uint8_t *)phys_to_virt(code_phys);

    /* 读取文件内容 */
    rc = ext4_fread(&file, code_buffer, file_size, &rcnt);
    if (rc != EOK || rcnt != file_size) {
        KLOG_ERROR("[loader] Failed to read file: rc=%d, rcnt=%zu\n", rc, rcnt);
        pmm_free_pages(g_pmm, code_phys, page_count);
        ext4_fclose(&file);
        return -4;
    }

    ext4_fclose(&file);

    KLOG_INFO("[loader] Program loaded: phys=0x%llx, virt=0x%llx\n",
              code_phys, (uint64_t)code_buffer);

    /* 创建用户进程
     * process_create 的 user_entry 参数在 AArch64 下是代码的内核虚拟地址，
     * vm_create_user_process 会从该地址拷贝代码到新的用户页表中。
     * 进程实际入口固定为用户虚拟地址 0x10000。
     */
    new_task = process_create(path_buf, (uint64_t)code_buffer, file_size,
                              USER_STACK_TOP, USER_PROCESS_PRIO);
    if (new_task == NULL) {
        KLOG_ERROR("[loader] Failed to create process for '%s'\n", path_buf);
        pmm_free_pages(g_pmm, code_phys, page_count);
        return -5;
    }

    KLOG_INFO("[loader] Process '%s' created successfully, PID=%u\n",
              path_buf, new_task->id);

    /* 退出当前进程，让新进程运行 */
    task_exit();

    /* 不应该到达这里 */
    return -6;
}
