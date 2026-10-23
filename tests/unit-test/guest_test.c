/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   GUEST_LINUX=1：直启 Linux guest。

   构建指纹 —— `make test-guest-linux` 靠它辨认 GUEST_LINUX 变体。
   不能用 KLOG 字符串：LOG=none 会把日志连同字符串一起编译掉。
   `used` 保证它落到 .rodata；token 只出现这一次（Makefile 按“恰好 1 次”
   校验）。
*/

#include "../boot_mode.h"
#include "klog.h"
#include "vmm/vmm.h"
#include "guest_loader.h"

__attribute__((used)) static const char g_build_tag_guest_linux[] = "GUEST_LINUX_BUILD_TAG";

bool test_boot_active(void)
{
    return true;
}

bool test_boot_skip_net(void)
{
    return true;
}

void test_boot_run(void)
{
    KLOG_INFO("=== GUEST_LINUX mode: booting Linux as EL1 guest ===\n");

    /* 直启模式：从 VM 池里取一个槽位（多 VM 之后直启也用同一条路径） */
    vm_t *vm = vm_alloc();
    if (!vm) {
        KLOG_ERROR("GUEST_LINUX: no free VM slot\n");
        return;
    }

    vm->cfg.mem_base = GUEST_LINUX_MEM_BASE;
    vm->cfg.mem_size = GUEST_LINUX_MEM_SIZE;
    /*
       直启没有命令行，沿用构建期的 GUEST_VCPUS。
       运行期指定（vmm-run -c N）只在 /dev/vmm 那条路上。
    */
    vm->cfg.nr_vcpus = GUEST_LINUX_NR_VCPUS;
    if (guest_loader_run_linux(vm) != 0)
        KLOG_ERROR("guest_loader_run_linux failed\n");
}
