# Avatar OS 测试指南

本文档描述项目的测试体系、各类测试的运行方式，以及如何添加新测试。

---

## 快速开始

```bash
# 默认模式：启动 busybox 交互 shell
make ARCH=aarch64 run-fs LOG=warn

# pthread 测试（含无锁竞争对照）
bash apps/c/build.sh                     # 首次：编译动态链接 rootfs（三架构）
make ARCH=riscv64 test-pthread LOG=warn  # QEMU 启动后运行 /bin/pthread_test

# mutex 测试（用户态 futex 自实现 mutex，含可重入 rmutex）
make ARCH=riscv64 test-mutex LOG=warn    # QEMU 启动后运行 /bin/mutex_test

# VMM 三线程上下文切换测试
make ARCH=aarch64 test-vmm LOG=info

# 内核单元测试（编译进内核，启动时自动运行）
# 见"内核单元测试"一节，在 kernel/main.c 中调用对应函数
```

---

## 测试体系概览

```
Avatar OS 测试
├── 1. 内核单元测试        tests/*.c         编译进内核，启动时运行
├── 2. 构建变体测试        kernel/main.c     通过 #ifdef RUN_XXX_TEST 切换
│   ├── PMM_TESTS=1        物理内存分配器
│   └── VMM_TEST=1         VMM 三线程上下文切换
├── 3. apps/ 汇编小程序    apps/<arch>/*.S   三种加载方式（见下节）
└── 4. 用户态 pthread 测试 apps/c/           动态链接 musl，完整 Linux ABI
```

---

## 一、内核单元测试（`tests/`）

### 已有测试文件

| 文件 | 入口函数 | 测试内容 |
|------|----------|----------|
| `tests/arch_test.c` | `run_arch_tests()` | 架构检测宏验证 |
| `tests/assert_test.c` | `run_assert_tests()` | 断言系统 |
| `tests/klog_test.c` | `run_klog_tests()` | 日志级别和格式化 |
| `tests/list_test.c` | `run_list_tests()` | 双向链表操作 |
| `tests/string_test.c` | — | 字符串函数 |
| `tests/spinlock_test.c` | — | 自旋锁 |
| `tests/mutex_test.c` | `run_mutex_tests()` | 基础互斥锁 |
| `tests/mutex_stress_test.c` | `run_mutex_stress_test()` | 互斥锁压力测试 |
| `tests/mutex_comparison_test.c` | `run_mutex_comparison_test()` | 有锁/无锁对比 |
| `tests/pmm_test.c` | `run_pmm_tests()` | 物理内存管理器 |
| `tests/vmm_test.c` | `run_vmm_test()` | VMM 三线程切换（见第二节）|

### 运行方式

所有 `tests/*.c` 文件**已自动编译链接进内核**，不需要额外 Makefile 操作。
要运行某个测试，在 `kernel/main.c` 的 `task_init()` 之后调用对应函数即可：

```c
// kernel/main.c，task_init() 之后，模式选择 #if 块之前
run_klog_tests();
run_list_tests();
run_mutex_tests();
```

运行完毕后通常继续正常启动（或调用 `platform_shutdown()` 关机）。

### 添加新内核单元测试

1. 在 `tests/` 目录新建 `foo_test.c`：

```c
// tests/foo_test.c
#include "klog.h"

void run_foo_tests(void) {
    KLOG_INFO("[foo_test] start\n");
    // 你的断言 / 逻辑
    KLOG_INFO("[foo_test] PASS\n");
}
```

2. 在 `kernel/main.c` 的 `task_init()` 后调用：

```c
extern void run_foo_tests(void);
run_foo_tests();
```

无需修改 Makefile —— `TESTS_OBJECTS` 规则会自动扫描 `tests/*.c`。

---

## 二、构建变体测试（`kernel/main.c` 模式切换）

### 工作原理

`kernel/main.c` 末尾用 `#if defined(RUN_XXX)` 选择启动模式，**所有模式共用同一套 idle 尾部**（抢占使能 → 切换 idle 栈 → wfe/wfi/hlt 循环）：

```c
task_init();

#if defined(RUN_VMM_TEST)
    run_vmm_test();
#elif defined(RUN_FOO_TEST)   // ← 新测试加在这里
    run_foo_test();
#else
    task_create("busybox", demo_load_busybox, NULL, 5);  // 默认
#endif

timer_set_tick_cb(sched_tick);   // 共用
task_switch_to_idle_stack();     // 共用
while (1) { task_yield(); wfe/wfi/hlt; }
```

Makefile 里有对应的变体追踪机制：**切换变体时自动强制重编 `kernel/main.c`**，无需手动 `make clean`。

### 现有构建变体

| 命令 | 宏定义 | 启动模式 |
|------|--------|----------|
| `make ARCH=xxx run-fs` | —（默认）| busybox 交互 shell |
| `make ARCH=xxx test-pthread` | —（默认）| busybox shell + pthread_test rootfs |
| `make ARCH=xxx VMM_TEST=1 kernel` | `RUN_VMM_TEST` | VMM 三线程上下文切换 |
| `make ARCH=xxx test-vmm` | `RUN_VMM_TEST`（自动）| 同上，一步完成 |
| `make ARCH=xxx RUN_PMM_TESTS=1 kernel`（需手动加 CFLAGS） | `RUN_PMM_TESTS` | PMM 测试后关机 |

### 添加新构建变体测试

**步骤一：Makefile 加变体选项**（仿 VMM_TEST 块，位于 `CFLAGS += -MMD -MP` 后的"构建变体"区域）：

```makefile
FOO_TEST ?= 0
ifeq ($(FOO_TEST),1)
    CFLAGS += -DRUN_FOO_TEST=1
    _BUILD_VARIANT := foo_test   # ← 修改此处唯一标识
endif
```

**步骤二：`kernel/main.c` 加 `#elif` 分支**：

```c
#elif defined(RUN_FOO_TEST)
    extern void run_foo_test(void);
    run_foo_test();
```

**步骤三：添加便捷 Makefile target**（仿 `test-vmm`）：

```makefile
test-foo:
    $(MAKE) ARCH=$(ARCH) LOG=$(LOG) ASSERT=$(ASSERT) FOO_TEST=1 kernel
    @echo "Starting QEMU for $(ARCH) — foo test..."
    $(QEMU) $(QEMU_FLAGS)
```

并在 `.PHONY` 行添加 `test-foo`。

之后运行：

```bash
make ARCH=aarch64 test-foo LOG=info
```

---

## 三、`apps/` 汇编小程序（三种加载方式）

`apps/` 目录下的汇编程序是内核测试的"用户端"，分三种加载方式：

### 方式 A：直接链接进内核（符号引用）

**特点**：程序代码作为 `.o` 文件链接进内核镜像，通过 C 函数指针直接调用/传递给任务。无文件系统依赖，重启后立即可用。

**文件规则**：

| 程序 | 源文件 | 对象文件 | 用途 |
|------|--------|----------|------|
| `user_test_program` | `apps/<arch>/user_test.S` | `build/user_test.o` | syscall 基础测试（write/getpid/yield/exit）|
| `el0_loop_program` | `apps/aarch64/el0_loop.S` | `build/apps_el0_loop.o` | EL0 无限 yield 循环（VMM 测试 Thread 3）|
| `guest_test_entry` | `apps/aarch64/guest_test.S` | `build/apps_guest_test.o` | EL1 guest 循环（VMM 测试 Thread 2）|
| `hello_program` | `apps/<arch>/hello.S` | `build/hello.o` | 最简 hello world 验证 |
| `x86_guest_test_entry` | `apps/x86_64/guest_test.S` | `build/apps_guest_test.o` | x86_64 VMX guest |

**如何调用**（在内核 C 代码中）：

```c
extern void user_test_program(void);   // 声明符号
extern void el0_loop_program(void);

// 作为内核任务运行（内核态）
task_create("user_test", user_test_program, NULL, 5);

// 作为用户进程运行（EL0/Ring3，需配合 process_create）
process_create("el0_loop", (uint64_t)el0_loop_program,
               0x4000,     // 代码大小（覆盖函数体）
               0x200000,   // 用户栈顶
               5);
```

**如何添加**：

1. 在 `apps/<arch>/foo.S` 编写程序，导出符号 `foo_program`
2. 在 Makefile 中添加对象文件到 `KERNEL_OBJECTS`，或参考 `TASK_USER_TEST_OBJ` 变量
3. 在内核 C 代码中 `extern void foo_program(void);` 后直接使用

---

### 方式 B：独立 `.bin` 文件，通过 `bin_loader` 从文件系统加载

**特点**：程序用 `app.ld`（入口 `_start`，链接到虚拟地址 `0x10000`）编译成**平坦二进制**，写入 rootfs，运行时由内核 `bin_loader_load_from_file()` 读取并用 `process_create` 创建用户进程。

**文件规则**（Makefile 自动处理 `apps/<arch>/*.S` 中未被方式 A 特殊处理的文件）：

```
apps/aarch64/test_exec.S
  → build/apps_test_exec.o    (编译，加 -DAPP_ELF=1)
  → build/test_exec.bin.elf   (链接，-T app.ld)
  → build/test_exec.bin       (objcopy -O binary)
```

**写入 rootfs**：

```bash
# 挂载 rootfs 镜像，写入 bin 文件
sudo mount -o loop build/rootfs-aarch64.img /mnt/tmp
sudo cp build/test_exec.bin /mnt/tmp/test_exec
sudo umount /mnt/tmp
```

**内核侧调用**：

```c
// 在某个内核任务中（如 demo_load_busybox 的变体）
bin_loader_load_from_file("/test_exec", NULL, NULL);
// bin_loader 调用 process_create，程序在 0x10000 开始运行，然后 task_exit()
```

**如何添加**：

1. 编写 `apps/<arch>/foo.S`，入口为 `_start`，使用自定义 syscall ABI（`svc #0` / `ecall` / `syscall`）
2. `make ARCH=<arch> kernel` 时自动生成 `build/foo.bin`
3. 将 `build/foo.bin` 写入 rootfs，通过 `bin_loader_load_from_file("/foo", ...)` 加载

---

### 方式 C：动态链接 musl ELF，通过 `elf_loader` 从文件系统加载

**特点**：使用 musl 交叉编译器编译 C 程序，动态链接 `ld-musl-<arch>.so.1`，写入 rootfs（含 busybox + musl libc）。内核通过 `elf_loader_load_from_file()` 加载，支持完整的 Linux syscall ABI（futex、clone、membarrier 等）。

这是**最接近真实 Linux 用户态**的测试方式。

**现有程序**：

| 程序 | 源文件 | 测试内容 |
|------|--------|----------|
| `pthread_test` | `apps/c/pthread/test.c` | pthread mutex、条件变量、无锁竞争演示 |
| `mutex_test` | `apps/c/mutex/test.c` | futex 自实现 `umutex_t`（不可重入）+ `rmutex_t`（可重入）|
| `busybox` | 预编译 `apps/busybox-<arch>` | 交互 shell、文件系统操作 |

**编译流程（一次性）**：

```bash
# 编译 pthread_test 到三架构，生成 imgs/rootfs-<arch>.img
bash apps/c/build.sh

# 之后用便捷 target 测试
make ARCH=riscv64 test-pthread LOG=warn
make ARCH=aarch64 test-pthread LOG=warn
make ARCH=x86_64  test-pthread LOG=warn
```

QEMU 启动后在 busybox shell 执行：

```sh
/bin/pthread_test
/bin/mutex_test
```

**如何添加新 C 用户程序**：

1. 在 `apps/c/<name>/` 下创建 `test.c`（使用标准 musl 头文件）
2. 在 `apps/c/build.sh` 的 `main()` 中添加 `build_prog` 调用：

```bash
# apps/c/build.sh main() 中添加
build_prog "$arch" "${CC_PREFIX[$arch]}" "${LD_INTERP[$arch]}" \
    "$SCRIPT_DIR/foo/test.c" "foo_test" || arch_ok=0
```

3. 重新运行 `bash apps/c/build.sh` 重建三架构 `imgs/rootfs-<arch>.img`
4. `make ARCH=aarch64 test-pthread` 后在 shell 中运行 `/bin/foo_test`

---

## 四、用户态 pthread 测试（`apps/c/pthread/test.c`）详解

pthread_test 包含 6 个测试（Test 0–5），覆盖从竞争态演示到完整多线程同步：

| 测试 | 内容 | 验证点 |
|------|------|--------|
| Test 0 | 无锁竞争演示（`sched_yield` 强制 read→yield→write 竞争）| 结果 < expected，证明竞争真实发生 |
| Test 1 | mutex 基础加解锁 | 单线程正确性 |
| Test 2 | 多线程 mutex 保护计数器 | `actual == expected`，无数据丢失 |
| Test 3 | 生产者/消费者（mutex + cond）| 所有 item 恰好消费一次 |
| Test 4 | 高并发压力（`T4_THREADS` 线程 × `T4_LOOPS` 次）| 计数无竞争丢失 |
| Test 5 | spinlock 互斥 | spinlock 等价于 mutex 的正确性 |

**单核 QEMU 典型输出（riscv64）**：

```
[Test 0] RACE DEMO: no lock
  expected=8000, actual=2000, lost=6000 (75%)  ← 竞争已确认
[Test 1] mutex basic: PASS
[Test 2] mutex counter: expected=80000, actual=80000: PASS
[Test 3] producer-consumer: PASS
[Test 4] stress: expected=40000, actual=40000: PASS
[Test 5] spinlock: PASS
ALL TESTS PASSED
```

---

## 四-B、用户态 mutex 测试（`apps/c/mutex/test.c`）详解

展示如何用内核的 `futex` syscall 从零实现 mutex，这正是 `pthread_mutex` 的底层机制。

### umutex_t — 不可重入 futex mutex

三态状态机：

| `state` | 含义 |
|---------|------|
| 0 | 未锁定 |
| 1 | 已锁定，无等待者（fast path）|
| 2 | 已锁定，有等待者（需要 wake）|

- **lock**：CAS(0→1) 快速路径；失败则 CAS(→2) 后 `FUTEX_WAIT(2)` 睡眠
- **unlock**：`fetch_sub(1)` → 若原值为 1 说明无等待者直接返回；否则 `store(0)` + `FUTEX_WAKE(1)`

### rmutex_t — 可重入（递归）mutex

在 `umutex_t` 基础上增加 `owner`（`pthread_self()` 转 `size_t`）和 `count` 字段：

- **lock**：`owner == self` 则 `count++` 直接返回；否则获取底层 `umutex_t`，设 `owner = self`，`count = 1`
- **unlock**：`--count > 0` 则直接返回；否则清零 `owner` 后释放底层锁

### 测试项

| 测试 | 内容 | 验证点 |
|------|------|--------|
| Test 0 | 无锁竞争演示 | 同 pthread_test，证明竞争真实发生 |
| Test 1 | umutex 基础单线程 lock/unlock/trylock | 状态值正确 |
| Test 2 | umutex 多线程计数器（4线程 × 20000 次）| `actual == expected` |
| Test 3 | rmutex 单线程递归加锁 3 次，解锁 3 次 | count 变化 + 锁状态正确 |
| Test 4 | rmutex 多线程 + 递归深度 2 | `actual == expected` |
| Test 5 | umutex trylock 竞争：acquired + failed == total | 计数完整性 |



## 五、VMM 三线程上下文切换测试（`tests/vmm_test.c`）详解

测试目标：验证调度器能在三类不同特权级的线程之间正确切换。

### AArch64（EL2 VHE）

| 线程 | 函数 | 特权级 | 输出 |
|------|------|--------|------|
| Thread 1 | `el2_loop_thread` | EL2（VHE 内核）| `[el2_loop] tick=N` 每 20 次 yield |
| Thread 2 | `vcpu_task_create` + `guest_test_entry` | EL1 guest（Stage-2）| `[guest] iter=N`（通过 HVC_PRINT）|
| Thread 3 | `process_create` + `el0_loop_program` | EL0 用户态 | `[el0] loop tick (EL0)` 每 50 次 yield |

### RISC-V（H 扩展）

| 线程 | 特权级 | 输出 |
|------|--------|------|
| Thread 1 `rv_host` | HS-mode | `[rv_host] tick=N` |
| Thread 2 vCPU | VS-mode guest | `[guest] iter=N` |
| Thread 3 `u_loop` | U-mode | `[u_loop] tick=N` |

### 运行命令

```bash
make ARCH=aarch64 test-vmm LOG=info
make ARCH=riscv64  test-vmm LOG=info
make ARCH=x86_64   test-vmm LOG=info
```

正常输出应见三路消息交替出现（频率不同），证明调度器在三种特权级之间正确切换。

---

## 附录：`apps/<arch>/` 文件速查

### AArch64

| 文件 | 加载方式 | 用途 |
|------|----------|------|
| `user_test.S` | A（链接进内核）| syscall 基础测试 |
| `hello.S` | A（链接进内核）| 最简 hello world |
| `el0_loop.S` | A（链接进内核）| VMM Test Thread 3（EL0 循环）|
| `guest_test.S` | A（链接进内核）| VMM Test Thread 2（EL1 guest）|
| `test_exec.S` | B（.bin 文件系统）| bin_loader 测试 |

### RISC-V 64

| 文件 | 加载方式 | 用途 |
|------|----------|------|
| `user_test.S` | A（链接进内核）| syscall 基础测试 |
| `hello.S` | A（链接进内核）| 最简 hello world |
| `guest_test.S` | A（链接进内核）| VMM Test Thread 2（VS-mode guest）|

### x86_64

| 文件 | 加载方式 | 用途 |
|------|----------|------|
| `user_test.S` | A（链接进内核）| syscall 基础测试 |
| `hello.S` | A（链接进内核）| 最简 hello world |
| `guest_test.S` | A（链接进内核）| VMM Test Thread 2（VMX non-root）|
| `test_execve.S` | A（链接进内核）| execve 系统调用测试 |

---

## 附录：完整测试命令速查

```bash
# ── busybox 交互 shell（默认）──────────────────────────────
make ARCH=aarch64 run-fs LOG=warn
make ARCH=riscv64  run-fs LOG=warn
make ARCH=x86_64   run-fs LOG=warn

# ── pthread 测试（动态 musl，首次需先 build.sh）───────────
bash apps/c/build.sh                      # 编译三架构 rootfs（一次性）
make ARCH=aarch64 test-pthread LOG=warn   # 启动后: /bin/pthread_test
make ARCH=riscv64  test-pthread LOG=warn
make ARCH=x86_64   test-pthread LOG=warn

# ── VMM 三线程切换测试 ────────────────────────────────────
make ARCH=aarch64 test-vmm LOG=info
make ARCH=riscv64  test-vmm LOG=info
make ARCH=x86_64   test-vmm LOG=info

# ── 内核单元测试（在 main.c 里调用后正常编译）────────────
make ARCH=aarch64 run LOG=debug           # 无 rootfs，看内核输出
```
