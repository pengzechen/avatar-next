# x86_64 启动 Busybox 实现路线图

**日期**: 2026-05-05  
**目标**: 使 x86_64 内核能够像 AArch64 和 RISC-V 一样启动 busybox  
**参考提交**: c840369, 15c8696, 2ccda3b, 84d064a

---

## 当前状态

### ✅ 已完成
1. 内核基础设施（PMM、调度器、timer、中断）
2. 文件系统（lwext4）
3. 内核任务切换
4. Idle 栈修复（刚完成）

### ❌ 缺失功能
1. 用户态支持（Ring 3 切换）
2. 系统调用处理（syscall/sysret）
3. 用户页表管理（CR3 切换）
4. ELF 加载器（x86_64 特定处理）
5. 用户进程跳板函数（task_trampoline_user）

---

## 实现步骤

### 第一阶段：用户态支持基础

#### 1.1 实现用户态异常处理（boot/x86_64/exception.S + exception.c）

**参考**: 
- AArch64: `boot/aarch64/exception.S` (SAVE_REGS/RESTORE_REGS)
- RISC-V: `boot/riscv64/exception.S` (trap_vector)

**需要实现**:
```asm
/* exception.S */
- 保存/恢复所有通用寄存器（RAX-R15）
- 保存/恢复用户栈指针（RSP from user mode）
- 保存 CS、SS、RFLAGS（由 CPU 自动压栈）
- 区分内核态/用户态中断源（检查 CS 的 RPL）
```

**异常帧结构**:
```c
typedef struct {
    uint64_t r15, r14, r13, r12;
    uint64_t r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rbx;
    uint64_t rdx, rcx, rax;
    uint64_t vector, error_code;
    /* CPU 自动压栈 */
    uint64_t rip, cs, rflags, rsp, ss;
} trap_frame_t;
```

**关键修改**:
- `boot/x86_64/exception.S`: 添加用户态异常入口
- `boot/x86_64/exception.c`: 
  - `handle_exception()` 区分用户/内核中断
  - 添加 `handle_user_exception()` 处理用户态异常

---

#### 1.2 实现系统调用入口（boot/x86_64/syscall_wrapper.S）

**x86_64 系统调用约定**:
- 指令: `syscall` / `sysret`
- 寄存器:
  - RAX: 系统调用号
  - RDI, RSI, RDX, R10, R8, R9: 参数 1-6
  - RCX: 保存返回地址（由 CPU 使用）
  - R11: 保存 RFLAGS（由 CPU 使用）
  - RAX: 返回值

**需要实现**:
```asm
.global syscall_entry
syscall_entry:
    /* 1. 保存用户上下文 */
    swapgs                     /* 切换 GS 到内核数据段 */
    mov    %rsp, %gs:user_rsp  /* 保存用户 RSP */
    mov    %gs:kernel_rsp, %rsp /* 切换到内核栈 */
    
    /* 2. 保存寄存器 */
    push   %rcx                /* 用户 RIP */
    push   %r11                /* 用户 RFLAGS */
    push   %rax, %rbx, ..., %r15
    
    /* 3. 调用 C 处理函数 */
    mov    %rsp, %rdi          /* trap_frame_t* */
    call   syscall_handler
    
    /* 4. 恢复用户上下文 */
    pop    %r15, ..., %rax
    pop    %r11                /* RFLAGS */
    pop    %rcx                /* RIP */
    mov    %gs:user_rsp, %rsp
    swapgs
    sysret
```

**MSR 配置**:
```c
/* exception.c: exception_init() */
wrmsr(MSR_STAR,  /* CS/SS 选择子 */);
wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);  /* syscall 入口 */
wrmsr(MSR_SFMASK, 0x200);  /* 清除 IF (关中断) */
```

**关键文件**:
- 新建: `boot/x86_64/syscall_wrapper.S`
- 修改: `boot/x86_64/exception.c` (配置 MSR)
- 修改: `kernel/syscall/syscall.c` (添加 x86_64 处理)

---

#### 1.3 实现用户任务跳板（kernel/task/x86_64/switch.S）

**当前状态**: `task_trampoline_user` 是空存根

**需要实现**:
```asm
.global task_trampoline_user
task_trampoline_user:
    /* 从内核栈获取用户入口和栈指针 */
    pop    %rdi                /* user_entry */
    pop    %rsi                /* user_sp */
    
    /* 准备 iretq 栈帧 */
    push   $0x23               /* SS (用户数据段 + RPL=3) */
    push   %rsi                /* RSP (用户栈) */
    pushfq                     /* RFLAGS */
    or     $0x200, (%rsp)      /* 设置 IF (开中断) */
    push   $0x2B               /* CS (用户代码段 + RPL=3) */
    push   %rdi                /* RIP (用户入口) */
    
    /* 清空通用寄存器（安全考虑） */
    xor    %rax, %rax
    xor    %rbx, %rbx
    ...
    xor    %r15, %r15
    
    /* 跳转到用户态 */
    iretq
```

**段描述符配置** (boot/x86_64/gdt.c):
- 0x18: Kernel Code (DPL=0)
- 0x20: Kernel Data (DPL=0)
- 0x28: User Code   (DPL=3, base=0x08 | RPL=3 = 0x2B)
- 0x30: User Data   (DPL=3, base=0x08 | RPL=3 = 0x23)

**关键修改**:
- `kernel/task/x86_64/switch.S`: 实现 `task_trampoline_user`
- 可能需要新建 `boot/x86_64/gdt.c`: 配置 GDT（如果还没有）

---

### 第二阶段：内存管理

#### 2.1 实现用户页表创建（kernel/mm/vm_user.c）

**参考**: 
- AArch64: `kernel/mm/aarch64/vmm.c` (vm_create_user_process)
- RISC-V: `kernel/mm/riscv64/mmu.S` (页表切换)

**x86_64 页表结构**:
- 4 级页表: PML4 → PDPT → PD → PT
- 页大小: 4KB
- 内核映射: 0xffff800000000000 - 0xffffffffffffffff (高 128TB)
- 用户映射: 0x0000000000000000 - 0x00007fffffffffff (低 128TB)

**需要实现**:
```c
/* vm_user.c */
uint64_t* vm_create_user_process(uint64_t user_start, uint64_t user_end,
                                  uint64_t stack_addr, uint64_t stack_size)
{
    /* 1. 分配 PML4 页表 */
    uint64_t *pml4 = pmm_alloc_page();
    memset(pml4, 0, 4096);
    
    /* 2. 复制内核页表项（高半部分）*/
    extern uint64_t *kernel_pml4;
    for (int i = 256; i < 512; i++) {
        pml4[i] = kernel_pml4[i];
    }
    
    /* 3. 映射用户代码段（0x10000 - user_end）*/
    /* 4. 映射用户栈（stack_addr - stack_addr + stack_size）*/
    /* 5. 设置页表属性（U/S=1, R/W=1）*/
    
    return pml4;  /* 物理地址 */
}
```

**关键修改**:
- `kernel/mm/vm_user.c`: 添加 x86_64 分支
- `include/x86_64/mm_vm.h`: 定义页表结构和标志

---

#### 2.2 实现页表切换（kernel/task/sched.c）

**参考**: RISC-V 的 `sched_schedule()` 中的 SATP 切换

**需要实现**:
```c
/* sched.c: sched_schedule() */
#if ARCH_X86_64
    if (next->is_user_process && next->pgd != 0) {
        /* 切换到用户页表 */
        uint64_t cr3 = (uint64_t)next->pgd;  /* 物理地址 */
        __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
    } else if (next->is_user_process == 0 && prev->is_user_process != 0) {
        /* 切换回内核页表 */
        extern uint64_t *kernel_pml4_phys;
        __asm__ volatile("mov %0, %%cr3" :: "r"(kernel_pml4_phys) : "memory");
    }
#endif
```

**关键修改**:
- `kernel/task/sched.c`: 添加 x86_64 页表切换
- `boot/x86_64/boot.S`: 导出 `kernel_pml4_phys`

---

### 第三阶段：ELF 加载和测试

#### 3.1 测试简单用户程序

**创建测试程序** (apps/x86_64/hello.S):
```asm
.section .text
.global _start
_start:
    /* SYS_WRITE (1, "Hello", 5) */
    mov    $1, %rax        /* syscall number */
    mov    $1, %rdi        /* fd = stdout */
    lea    msg(%rip), %rsi /* buf */
    mov    $5, %rdx        /* count */
    syscall
    
    /* SYS_EXIT (0) */
    mov    $60, %rax
    xor    %rdi, %rdi
    syscall

.section .rodata
msg:
    .ascii "Hello"
```

**编译脚本**:
```bash
x86_64-linux-musl-gcc -nostdlib -static \
    -Wl,-Ttext=0x10000 -o hello hello.S
```

**测试步骤**:
1. 在 `kernel/main.c` 中创建测试任务
2. 使用 `process_create()` 加载用户程序
3. 验证系统调用输出
4. 验证进程退出和调度

---

#### 3.2 修复 ELF 加载器（kernel/loader/elf_loader.c）

**可能需要的修改**:
- 处理 x86_64 ELF 魔数和架构标识
- 调整用户程序加载地址（0x10000）
- 验证段对齐和权限

**关键问题** (参考 RISC-V 提交):
- 全局指针（GP）问题 (x86_64 无 GP，但需注意 PIE/PIC)
- 页表在系统调用中被破坏 → 需要在异常处理中保护 CR3

---

#### 3.3 启动 busybox

**准备工作**:
1. 交叉编译 busybox for x86_64
   ```bash
   make ARCH=x86_64 CONFIG_STATIC=y
   ```

2. 安装到 rootfs:
   ```bash
   ./install-apps.sh ARCH=x86_64
   ```

3. 在 `kernel/main.c` 中启用 busybox 加载:
   ```c
   #elif ARCH_X86_64
       task_t *proc1 = task_create("busybox_loader", demo_load_busybox, NULL, 5);
   ```

**预期问题** (参考 RISC-V):
- 系统调用兼容性（musl libc 使用的 syscall）
- 内存管理（PMM 值被破坏？）→ 保护全局变量
- 中断上下文切换（GP/CR3 保存/恢复）

---

## 文件修改清单

### 必须修改
- [x] `include/x86_64/mmu.h` (新建) - 完善的页表定义和宏
- [x] `boot/x86_64/exception.c` - MSR 配置、SYSCALL/SYSRET 支持
- [x] `boot/x86_64/syscall_wrapper.S` (新建) - syscall/sysret 入口
- [x] `boot/x86_64/boot.S` - 添加用户段到 GDT
- [x] `kernel/task/x86_64/switch.S` - task_trampoline_user 实现
- [x] `include/x86_64/syscall_abi.h` - 系统调用 ABI（已存在）
- [x] `kernel/task/task.c` - 修复 idle 栈切换问题
- [ ] `kernel/syscall/syscall.c` - x86_64 系统调用处理（已支持）
- [ ] `kernel/task/sched.c` - CR3 切换
- [ ] `kernel/mm/vm_user.c` - x86_64 页表创建

### 可能需要修改
- [x] `boot/x86_64/gdt.c` - 用户段描述符（已在 boot.S 中添加）
- [ ] `kernel/loader/elf_loader.c` - x86_64 特定处理
- [x] `include/x86_64/mm_vm.h` - 页表结构定义（已有占位实现）
- [ ] `kernel/main.c` - 启用 busybox 加载

### 参考文档
- [ ] `docs/RISCV64_ECALL_OPENSBI_BUG.md` - 异常处理陷阱
- [ ] `docs/RISCV64_PAGE_TABLE_SWITCH.md` - 页表切换问题
- [ ] `docs/INTERRUPT_CONTROL_COMPARISON.md` - 中断控制比较

---

## 调试检查点

### 第一阶段验证
1. ✅ 内核任务正常调度（task_a/b/c）— **已完成，idle 栈修复成功**
2. ⬜ 能够切换到用户态（Ring 3）— **待测试**
3. ⬜ 用户态能触发系统调用
4. ⬜ 系统调用能正常返回
5. ⬜ 用户进程能正常退出和重新调度

### 第二阶段验证
1. ⬜ 用户页表创建成功
2. ⬜ 页表切换不崩溃
3. ⬜ 用户程序能访问自己的地址空间
4. ⬜ 用户程序不能访问其他进程地址空间
5. ⬜ 内核映射在所有页表中可见

### 第三阶段验证
1. ⬜ 简单用户程序（hello.S）运行成功
2. ⬜ busybox 能加载到内存
3. ⬜ busybox 能执行简单命令（ls）
4. ⬜ busybox 能执行复杂命令（sh）
5. ⬜ 多进程调度稳定

---

## 常见问题和解决方案

### Q1: General Protection Fault 在系统调用返回时
**原因**: 段选择子错误或 RFLAGS 设置错误  
**解决**: 检查 CS/SS 的 RPL=3，RFLAGS.IF=1

### Q2: Page Fault 在用户态
**原因**: 页表未正确映射用户代码/数据/栈  
**解决**: 使用 CR2 查看访问地址，检查页表项

### Q3: 系统调用后全局变量值错误
**原因**: 异常处理中 CR3 被切换，全局变量基址错误  
**解决**: 参考 RISC-V 的 GP 修复，保存/恢复 CR3

### Q4: busybox 崩溃在特定系统调用
**原因**: 系统调用号或参数传递错误  
**解决**: 添加日志打印 RAX, RDI, RSI 等，比对 Linux syscall 表

---

## 估计工时

- 第一阶段（用户态基础）: 4-6 小时
- 第二阶段（内存管理）: 3-4 小时
- 第三阶段（ELF 和测试）: 2-3 小时
- **总计**: 9-13 小时

---

**开始时间**: 2026-05-05 12:30  
**预计完成**: 2026-05-06

**下一步**: 开始实现第一阶段 1.1 - 用户态异常处理
