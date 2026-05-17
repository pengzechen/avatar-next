/*
 * mm/pmap_compat.c - 进程地址空间相关的兼容/小型 syscall 处理器
 *
 * - arch_prctl_handler  (x86 only) — ARCH_SET_FS / ARCH_GET_FS
 * - getrandom_handler  — musl stack canary 等需要的伪随机
 * - x86_write_msr / x86_write_fs_base — MSR 写入辅助（x86）
 */
#include "syscall/syscall_internal.h"
#include "task/task.h"
#include "klog.h"

#if ARCH_X86_64
void x86_write_msr(uint32_t msr, uint64_t value)
{
    uint32_t lo = (uint32_t)(value & 0xFFFFFFFFU);
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" :: "c"(msr), "a"(lo), "d"(hi));
}

void x86_write_fs_base(uint64_t fs_base)
{
    x86_write_msr(X86_MSR_IA32_FS_BASE, fs_base);
}

void arch_prctl_handler(uint64_t regs[6], task_t *current)
{
    uint64_t code = regs[0];
    uint64_t addr = regs[1];

    if (code == X86_ARCH_SET_FS) {
        current->fs_base = addr;
        x86_write_fs_base(addr);
        regs[0] = 0;
    } else if (code == X86_ARCH_GET_FS) {
        if (addr == 0) {
            regs[0] = (uint64_t)(int64_t)-EFAULT;
        } else {
            *(uint64_t *)addr = current->fs_base;
            regs[0] = 0;
        }
    } else {
        regs[0] = (uint64_t)(int64_t)-EINVAL;
    }
}
#endif

void getrandom_handler(uint64_t regs[6])
{
    char     *buf = (char *)regs[0];
    uint64_t  len = regs[1];
    if (buf) {
        /* LCG 伪随机，以启动时间戳为种子。不是密码学安全的，
         * 但每次调用输出不同序列，满足 musl stack canary 等基本需求。 */
        static uint64_t s_prng_state = 0;
        if (s_prng_state == 0)
            s_prng_state = kernel_get_ns() ^ 0x9e3779b97f4a7c15ULL;
        for (uint64_t i = 0; i < len; i++) {
            s_prng_state = s_prng_state * 6364136223846793005ULL
                         + 1442695040888963407ULL;
            buf[i] = (char)(s_prng_state >> 56);
        }
    }
    regs[0] = len;
}
