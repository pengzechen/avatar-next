# 用户进程实现状态

## 当前状态（2026-05-02）

### ✅ 已完成功能

1. **用户进程创建**
   - `process_create()` 可以创建用户进程
   - 进程拥有独立的内核栈和用户栈
   - 进程可以加入调度队列并被调度执行

2. **特权级切换**
   - `arch_switch_to_user()` 正确实现从内核态（EL1）到用户态（EL0）的切换
   - 使用 `eret` 指令跳转到用户空间
   - 用户栈指针（SP_EL0）正确设置
   - 异常返回地址（ELR_EL1）和状态（SPSR_EL1）正确配置

3. **用户程序执行**
   - 用户程序在 EL0 运行
   - 系统调用（`svc #0`）正常工作
   - 进程可以通过 `SYS_YIELD` 主动让出 CPU
   - 进程可以被调度器重新调度

4. **系统调用处理**
   - `syscall_handler()` 正确处理系统调用请求
   - `SYS_WRITE`（0）：输出字符串
   - `SYS_YIELD`（2）：让出 CPU

### ⚠️ 临时限制

1. **页表共享**
   - 当前用户进程与内核共享页表（`task->pgd = NULL`）
   - 用户程序运行在内核虚拟地址空间
   - 所有进程可以看到相同的地址空间

2. **地址映射**
   - 用户程序链接在高地址（如 `0xffff0000400a6d50`）
   - 未实现地址重定位（relocation）
   - 用户栈映射在高地址（`0x70000000`）

### ❌ 未实现功能

1. **地址空间隔离**
   - 独立页表创建（`vm_create_user_process()`）已实现但未启用
   - 用户程序需要重新定位到低地址（如 `0x10000`）
   - 用户代码段、数据段、栈段的隔离映射

2. **用户程序加载器**
   - ELF 文件加载
   - 地址重定位
   - 动态链接（可选）

3. **进程隔离保护**
   - 用户空间访问权限检查
   - 内核空间保护
   - 进程间隔离

## 技术细节

### 用户进程创建流程

```c
task_t *proc1 = process_create("user_test",
                                (uint64_t)user_test_program,
                                0x70000000ULL,
                                10);
```

参数说明：
- `user_test_program`: 用户程序入口（内核虚拟地址）
- `0x70000000`: 用户栈虚拟地址
- `10`: 进程优先级

### 特权级切换流程

1. **内核态初始化**
   ```c
   task->sp = arch_init_user_stack(stack_base, stack_size,
                                   task->user_entry, task->user_sp);
   ```
   - 在内核栈上构造切换帧
   - 保存用户入口地址到 x19
   - 保存用户栈指针到 x20
   - 设置返回地址为 `task_trampoline_user`

2. **首次调度**
   - 调度器选择用户进程
   - `arch_task_switch()` 切换到用户进程的内核栈
   - `ret` 跳转到 `task_trampoline_user`

3. **跳转到用户态**
   ```asm
   task_trampoline_user:
       msr     daifclr, #2      /* 开启中断 */
       mov     x0, x19          /* user_entry */
       mov     x1, x20          /* user_sp */
       mov     x2, sp           /* kernel_sp */
       b       arch_switch_to_user

   arch_switch_to_user:
       msr     sp_el0, x1       /* 设置用户栈 */
       msr     elr_el1, x0      /* 设置返回地址 */
       mov     x9, #0x340
       msr     spsr_el1, x9     /* EL0t, IRQ使能 */
       eret                     /* 跳转到用户态 */
   ```

4. **用户程序执行**
   - CPU 执行用户代码
   - 使用 SP_EL0 作为栈指针
   - 访问用户数据段

5. **系统调用**
   ```asm
   mov     x8, #0              /* SYS_WRITE */
   svc     #0                  /* 触发异常 */
   ```
   - `svc` 指令触发异常
   - CPU 从 EL0 跳转到 EL1
   - `syscall_handler()` 处理请求
   - `eret` 返回用户空间

### 测试输出示例

```
[INFO] [task] created user process 'user_test' id=1 prio=10
[INFO] Process 1 (user_test) created successfully!
[user] Hello from user space!
[user] #
[user] #
[user] #
...
```

## 下一步工作

### 短期目标

1. **启用页表隔离**
   - 修改链接脚本，将用户程序链接到低地址（0x10000）
   - 或实现地址重定位，将用户程序复制到低地址
   - 在 `process_create()` 中启用独立页表创建

2. **完善地址空间布局**
   ```
   用户地址空间（TTBR0）：
     0x00000000 - 0x00FFFFFF: 用户代码段（只读，可执行）
     0x10000000 - 0x2FFFFFFF: 用户数据段（读写）
     0x70000000 - 0x700FFFFF: 用户栈（读写，不可执行）

   内核地址空间（TTBR1）：
     0xFFFF000000000000+: 内核代码和数据
   ```

3. **增加系统调用**
   - `SYS_EXIT`: 退出进程
   - `SYS_GETPID`: 获取进程 ID
   - `SYS_FORK`: 创建子进程
   - `SYS_EXEC`: 执行新程序

### 长期目标

1. **用户程序加载器**
   - ELF 解析器
   - 程序加载
   - 动态链接

2. **进程管理**
   - 进程树
   - 父子进程关系
   - 进程资源管理

3. **安全增强**
   - 用户空间访问检查
   - 内核空间保护
   - 进程间隔离

## 相关文件

- `kernel/task/task.c`: 进程创建和管理
- `kernel/task/aarch64/switch.S`: 上下文切换和特权级切换
- `kernel/task/aarch64/user_test.S`: 用户态测试程序
- `kernel/syscall/syscall.c`: 系统调用处理
- `kernel/mm/aarch64/vm_user.c`: 用户进程页表管理
- `kernel/mm/aarch64/vm_user.h`: 用户进程页表接口

## 调试命令

```bash
# 编译
make ARCH=aarch64 kernel

# 运行
make ARCH=aarch64 run

# GDB 调试
make ARCH=aarch64 debug
# 在另一个终端：
aarch64-linux-musl-gdb build/kernel_aarch64.elf
(gdb) target remote :1234
(gdb) break arch_switch_to_user
(gdb) continue
```

## 已知问题

1. **页表隔离未启用**
   - 影响：所有进程共享地址空间
   - 状态：临时限制
   - 计划：需要实现用户程序重定位

2. **用户程序地址固定**
   - 影响：用户程序必须链接到内核地址空间
   - 状态：临时限制
   - 计划：实现 ELF 加载器

3. **缺少进程销毁**
   - 影响：进程退出后资源未释放
   - 状态：未实现
   - 计划：实现 `task_exit()` 和资源清理

## 总结

当前实现已经完成了用户进程的核心功能：
- ✅ 进程创建
- ✅ 特权级切换
- ✅ 用户程序执行
- ✅ 系统调用处理

虽然页表隔离尚未实现，但系统已经可以在用户态运行程序，这是一个重要的里程碑。下一步将专注于实现地址空间隔离，以完成真正的进程隔离。
