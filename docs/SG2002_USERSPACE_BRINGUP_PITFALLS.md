# SG2002 用户态 Bring-up 坑点记录

日期：2026-06-03

本文记录这次在 SG2002 RISC-V 板子上跑通动态用户态、BusyBox、CVI sample、ION/TPU runtime 和 detector 多线程路径时遇到的主要问题、根因、修复方式，以及后续更合理的内核结构改造方向。

## 先说最后一个修复：是不是比较 hack

是，有一点，但它是一个命中当前根因的过渡修复。

当前 detector 的直接问题是：`CLONE_VM` 线程共享同一个用户页表，但 `task_t` 里仍然保存了每个线程私有复制出来的 `mmap_next` 和 `heap_end`。pthread 创建线程栈/TLS 后，线程 A 的 `mmap_next` 前进了，线程 B 还拿着 clone 时的旧值，于是线程 B 的 `mmap(NULL, ...)` 可能重新映射到线程 A 已经占用的地址，最后反复 `map failed` 并触发 `std::bad_alloc`。

这次补丁通过扫描所有同一 `pgd` 的 live task，取最大的 `mmap_next`/`heap_end`，成功后同步回同一线程组。它能解决当前 detector 失败，改动小，适合 bring-up 阶段先验证完整链路。

但正常设计不应该靠扫描 `g_task_pool` 同步 per-task 字段。正常应该引入真正的共享对象：

- `mm_struct`：保存 `pgd`、`mmap_base/mmap_next`、`brk_start/brk_end`、VMA 列表或树、地址空间锁、引用计数。
- `task->mm`：`CLONE_VM` 线程共享同一个 `mm`，fork/exec 创建或替换 `mm`。
- VMA 管理：`mmap(NULL)` 应查找地址空洞，而不是只靠单调 high-water 指针。
- `files_struct`：保存共享 fd table、锁、引用计数、close-on-exec 状态。
- 生命周期：最后一个线程退出时释放共享 `mm/files`，普通线程退出不释放进程级资源。

结论：最后一个修复是合理的 bring-up 过渡补丁，不是最终架构。后续应该替换为共享 `mm_struct`、VMA 管理和共享 `files_struct`。

## 当前目标和常用命令

目标平台：

```text
ARCH=riscv64 PLATFORM=sg2002-riscv64 ETH=none
```

构建并部署内核：

```sh
make ARCH=riscv64 PLATFORM=sg2002-riscv64 ETH=none kernel
cp build/kernel_riscv64.bin imgs/kernel_riscv64.bin
```

U-Boot 启动命令：

```sh
fatload mmc 0:1 0x89000000 rootfs.img  ; fatload mmc 0:1 0x80200000  kernel_riscv64.bin ; go 0x80200000
```

rootfs 是 64 MiB ext4 镜像，加载到 `0x89000000`。动态程序使用 `/lib/ld-musl-riscv64v0p7_xthead.so.1`。

用户地址布局：

```text
USER_CODE_BASE      = 0x10000
USER_INTERP_BASE    = 0x40000000
USER_MMAP_BASE_PIE  = 0x50000000
USER_STACK_TOP      = 0x70000000
USER_STACK_SIZE     = 0x100000
```

## 整体时间线

1. 根据 `sysroot` 生成 64 MiB `rootfs.img`。
2. 修正 `/busybox` 启动和 SG2002 U-Boot 加载命令。
3. 跑动态链接用户程序，修复 ELF loader 对动态 ELF 的错误 relocation。
4. 根据旧移植经验，在 SG2002 用户态入口补 T-Head C906 cache 同步。
5. 参考 StarryOS ION 实现，调整 Avatar 的 ION fd/handle 模型。
6. 兼容 CVI TPU legacy ioctl，使 `sample_gemm` 能走到 submit/wait/cache。
7. 修复 ION/device 用户映射被通用 VM teardown 错误释放的问题。
8. 对比能跑和崩溃日志，定位 ld-musl 概率崩溃为 RISC-V trap entry 污染用户 `t1`。
9. 实现 `readv` syscall 65，解决 OpenCV 读图失败。
10. 修复 detector 多线程匿名 mmap 撞线程栈导致 `std::bad_alloc`。
11. 修复线程退出关闭共享 fd 的生命周期问题。

## 坑 1：rootfs 镜像和启动地址

现象：rootfs 生成后 `/busybox` 启动失败，或板子没有看到预期文件。

根因：板端启动命令、平台宏、镜像文件必须一致。SG2002 当前 rootfs 通过 U-Boot 加载到 `0x89000000`，内核加载到 `0x80200000`。

修复：生成 64 MiB ext4 `rootfs.img`，并在 `imgs/readme.md` 记录正确 fatload/go 命令。

经验：板端 bring-up 先确认 kernel load address、rootfs load address、平台宏里的 rootfs base/size 是否一致。

## 坑 2：动态 ELF relocation 归属错误

现象：动态用户程序或 ld-musl 启动异常，musl relocation 路径不稳定。

根因：内核 loader 对带 `PT_INTERP` 的动态 ELF 或强制基址加载的解释器做了 RELA relocation，但这些 relocation 应由动态链接器处理。内核提前处理会破坏 musl 后续 relocation。

修复：`kernel/loader/elf_image.c` 中，当 `force_base != 0` 或主程序存在 interpreter 时跳过内核 RELA relocation。

经验：内核负责映射 segment 和构造 auxv；动态 ELF 的 relocation 交给 ld-musl。

## 坑 3：SG2002/T-Head C906 用户态 I-cache 可见性

现象：刚加载或写入的用户代码进入用户态后表现不稳定。

根因：SG2002 C906 上，仅 `fence.i` 对当前路径不够稳，旧移植版本在进入用户态前额外刷过 cache。

修复：在 RISC-V fresh exec、fork resume、trap return 到用户态前加入：

```asm
fence.i
.long 0x0100000b    /* ICACHE.IALL */
.long 0x01a0000b    /* SYNC.I */
```

经验：SG2002 上动态 loader、用户代码加载、fork resume 如果出现随机执行异常，先把 I-cache 同步纳入排查。

## 坑 4：fresh exec 继承旧 `tp`

现象：fresh exec 后可能带着内核或旧任务的 `tp`，影响用户 TLS 初始化。

根因：RISC-V `tp` 在用户 ABI 中是 TLS 指针，fresh exec 不能继承旧上下文。

修复：`kernel/task/riscv64/switch.S` 在第一次 `sret` 进入新用户镜像前执行 `mv tp, zero`。

经验：fork 可以继承用户寄存器，exec 要明确初始化用户 ABI 状态。

## 坑 5：RISC-V trap entry 污染用户 `t1`

现象：ld-musl `do_relocs` 附近概率性 Store page fault，有时同一个程序能跑，有时崩。寄存器日志显示 `t1` 异常变成用户栈地址。

根因：U -> S trap entry 中，在保存用户原始 `t1` 前先把 `t1` 当临时寄存器保存 `user_sp`，导致返回用户态后 `t1` 被污染。

修复：`boot/riscv64/exception.S` 中先用 `t0` 计算 trap frame 地址，立即保存原始 `sp` 和原始 `t1`，再从 `sscratch` 取回用户原始 `t0`。

经验：trap entry 里任何 GPR 在保存前都不能当 scratch 用。动态链接器概率崩溃不一定是 ELF 或 musl 问题，也可能是 trap frame 保存现场错了。

## 坑 6：ION ABI 不能把 handle 当 fd

现象：CVI runtime 期望 `/dev/ion` allocation 返回真实 fd，后续 mmap/ioctl/cache 操作用 fd 访问 buffer；Avatar 初始实现更像内部 handle。

根因：Linux/CVI ION ABI 是 fd-backed buffer 语义。handle、fd、dma buffer 生命周期不能混用。

修复：

- ION entry 增加 `ref_count`。
- 增加 `ion_ref()`。
- `/dev/ion` allocate 返回真实 fd。
- fd pool 支持 ION fd。
- `ION_IOC_FREE/GET/SIZE/IMPORT` 兼容 fd 到内部 handle 的转换。

经验：设备 ABI 要按用户态 runtime 的真实调用约定实现，不能因为内部 handle 是整数就当 fd 暴露。

## 坑 7：CVI TPU legacy ioctl 编码和参数布局

现象：`sample_gemm` 或 detector 在 TPU submit/wait/cache op 附近失败。

根因：CVI runtime 使用 legacy ioctl，请求号和参数布局如下：

```text
0x40087001 submit:             { i32 fd; u32 seq; }
0xc0087006 wait:               { u32 seq; i32 ret; }
0x40087002/03 fd cache op:     int32_t *fd
0x40087004/05 range cache op:  { u64 paddr; u64 size; i32 fd; }
```

修复：pseudofs 的 `/dev/cvi-tpu0` ioctl 兼容 legacy submit/wait/cache，请求里传 fd 时先映射到 ION handle，再找到 kernel VA/PA 做 cache clean/invalidate。

经验：厂商 runtime ABI 不能只靠本地头文件猜，要参考能工作的实现、真实日志或 ioctl 请求号。

## 坑 8：ION/device 映射不能被通用 VM teardown 当匿名页释放

现象：ION buffer 映射到用户态后，进程退出或 unmap 可能被通用 VM 路径 `pmm_free_pages`，破坏设备/ION 生命周期。

根因：用户页表里同时存在匿名页和外部设备映射，但 PTE/VMA 没有记录 leaf page 是否归 PMM 管。

修复：RISC-V PTE 增加 `RV_PTE_NOFREE`，`vm_user.c` 在 destroy/unmap leaf 时跳过带该标记的页。

经验：用户地址空间不是只有匿名内存。device/file/dma 映射必须携带生命周期/所有权信息。

## 坑 9：PMM 使用入口不统一

现象：部分路径直接使用 `pmm`，部分路径使用 `g_pmm`，跨架构和初始化阶段容易不一致。

修复：匿名 `mmap` 和 `brk` 统一使用 `g_pmm`。

经验：内存分配入口要统一，否则后续查页生命周期会非常痛苦。

## 坑 10：OpenCV/detector 使用 `readv` syscall 65

现象：detector 日志出现：

```text
Unknown syscall: 65
Could not open or find the image
```

根因：RISC-V Linux syscall 65 是 `readv`，OpenCV 图像读取路径会用它。底层 syscall 缺失会表现成上层库“读不到图片”。

修复：实现 `LINUX_SYS_READV` 和 `readv_handler`，遍历 iovec 并复用已有 `read_handler`，保留 partial read 语义。

经验：C++/OpenCV 程序覆盖的 Linux syscall 比 BusyBox 多。遇到应用层错误先确认 syscall 是否 ENOSYS。

## 坑 11：detector 多线程 `mmap` 撞线程栈

现象：detector 能完成模型注册、ION 分配、cache op，进入多线程后反复出现：

```text
[mmap] map failed: va=0x516b4000 flags=0x22
[mmap] map failed: va=0x516d7000 flags=0x22
std::bad_alloc
```

根因：`CLONE_VM` 共享页表，但 `mmap_next` 是 per-task 复制值。线程创建栈推进了一个 task 的 `mmap_next`，其他线程仍然从旧值开始匿名 mmap，撞到已映射的线程栈/TLS。

当前修复：

- 非 `MAP_FIXED` mmap 选址前取同一 `pgd` live task 的最大 `mmap_next`。
- mmap 成功后同步新的 high-water mark 到所有同 `pgd` task。
- `brk` 同步同一 `pgd` 的 `heap_end`。

正常修复方向：引入共享 `mm_struct`、VMA 管理、VM lock 和 refcount，让 `CLONE_VM` 线程真正共享地址空间元数据。

经验：支持 pthread 时，共享页表不够，地址空间元数据也必须共享。

## 坑 12：线程退出不能关闭共享 fd

现象：pthread 退出时如果遍历并释放 `fd_table`，会把主线程或其他线程仍在使用的 ION/TPU/文件 fd 关掉。

根因：线程退出和进程退出走了同一套 fd cleanup，但 `CLONE_FILES` 语义下 fd object 属于共享进程资源。

当前修复：`current->is_thread` 为真时，`sys_exit()` 不关闭所有 fd，只做 clear-tid/futex wake 和任务退出。非线程进程退出才清理 fd。

正常修复方向：引入 `files_struct`，fd table、fd object、dup、close、fork、thread exit 都通过引用计数管理。

经验：fd 是资源引用，不是每个 task 私有的整数数组。

## 坑 13：`tkill` 等小 syscall 也会被 runtime 用到

现象：pthread/C++ runtime 的 abort、signal、线程路径可能使用旧接口如 `tkill`。

修复：实现 `tkill`，按 tid 查找 task 并发送 signal。

经验：能跑 BusyBox 不代表能跑 libc++/OpenCV。runtime 的错误路径也需要 syscall 兼容。

## 坑 14：日志要打印完整寄存器现场

现象：ld-musl 概率崩溃只看 `pc/stval` 很难定位。

修复：RISC-V page fault 日志临时打印更多 GPR，包括 `ra/sp/gp/tp/t0/t1/t2`、`a0-a7`、`s*`、`t3-t6`。

经验：汇编 trap path 问题要看“本不该变化的寄存器”是否被内核污染。完整寄存器现场比只看 fault address 更有用。

## 已验证进展

- BusyBox/rootfs 路径已建立。
- 动态 ELF + ld-musl relocation 路径已修正。
- SG2002 用户态 cache 同步已补齐。
- ION fd 模型、TPU legacy ioctl、cache op 已支撑 `sample_gemm` 路径。
- ld-musl `do_relocs` 概率性崩溃定位为 trap entry 污染 `t1`，已修复。
- detector 的 `Unknown syscall: 65` 已通过 `readv` 修复。
- detector 的 pthread mmap 栈冲突已通过共享 high-water mark 过渡修复。
- 最新 SG2002 内核已构建并同步到 `imgs/kernel_riscv64.bin`，`cmp=0`，大小 `364736` 字节。

## 后续建议

1. 引入 `mm_struct`，替代 task 私有 `pgd/mmap_next/heap_end`。
2. 引入 VMA list/tree，让 `mmap/munmap/brk` 有真实区间管理。
3. 引入 `files_struct` 和 fd object 引用计数，正确支持 `CLONE_FILES`、fork、dup、close、exit。
4. 给 ION/device mmap 建模 VMA 类型和 close/unmap hooks，逐步替代只靠 PTE `NOFREE`。
5. detector 稳定后移除或降级临时超详细寄存器日志。
6. 把 SG2002 C906 cache sync 封装为平台 helper，避免 `.long` 分散在多个汇编路径。
7. 增加回归测试：动态 ELF、pthread stack mmap、ION fd 生命周期、TPU legacy ioctl、`readv` 图像读取。

## Commit Message 草稿

```text
riscv64: bring up SG2002 dynamic userspace and CVI runtime

- document SG2002 rootfs boot flow and add a 64 MiB rootfs image
- skip kernel RELA relocation for dynamic ELF/interpreter loads
- add SG2002 C906 user-entry I-cache synchronization
- fix RISC-V trap entry to preserve user t1 across U-mode traps
- clear tp on fresh RISC-V exec entry
- add ION fd/refcount compatibility and CVI TPU legacy ioctl handling
- mark external RISC-V user mappings as NOFREE during VM teardown
- add readv/tkill syscall compatibility used by OpenCV/pthread runtimes
- synchronize mmap/brk high-water marks across CLONE_VM threads
- avoid closing shared fd objects on pthread exit
```
