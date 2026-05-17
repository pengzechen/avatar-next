# busybox vi 卡死修复：poll / pselect6 / select 语义订正

## 现象

在 Avatar OS 上运行 busybox `vi` 时：

- 进入 vi 后 `hjkl` 不能移动光标
- 按 `ESC` 没反应
- `:q` / `:q!` 无法退出

但 `i` 进入插入模式、字符回显是正常的——说明 `read(0,&c,1)` 单字符路径本身已经通了。

## 根因分析

旧版 [`kernel/syscall/syscall.c`](../kernel/syscall/syscall.c) 里 `poll`/`ppoll`/`pselect6` 三个系统调用合并到一个分支，无条件把 **所有 fd 都谎报成 ready**：

```c
if (pfds[pi].fd == 0) {
    /* Pretend stdin always has data ready (non-blocking shell) */
    pfds[pi].revents = pfds[pi].events & 0x01;
    if (pfds[pi].revents) ready++;
} else if (pfds[pi].fd >= 0) {
    pfds[pi].revents = pfds[pi].events & 0x01;
    if (pfds[pi].revents) ready++;
}
```

vi 的按键流水线大致是：

1. 用户按 `ESC`（0x1B），UART → ring buffer
2. vi 的 `read(1)` 返回 0x1B
3. vi 怀疑这是功能键 escape 序列（`ESC [ A` 之类），调用
   `poll(stdin, POLLIN, 25ms)` 等待下一个字符
4. **谎报 ready 的 bug**：`poll` 立即返回 1，且不消耗任何时间
5. vi 紧接着调用 `read(0,&c,1)`，进入 UART 轮询阻塞
6. 用户后续按 `h/j/k/l/:` 等键时，vi 把它们当作 escape 序列的 **后续字节**
   吃掉；裸 `ESC` 永远不会超时返回，所以也无法回到 normal mode

同样的故事发生在 `:q!` 退出流程上——`:` 被当成 ESC 的下一字节。

另外 `pselect6` 的 ABI 和 `poll/ppoll` 完全不同：

| syscall    | regs[0]    | regs[1]    | regs[2]    | regs[3] | regs[4]   |
|------------|------------|------------|------------|---------|-----------|
| poll       | `pollfd*`  | `nfds`     | `timeout_ms` | —     | —         |
| ppoll      | `pollfd*`  | `nfds`     | `timespec*` | sigmask | sz        |
| pselect6   | `nfds`     | `readfds*` | `writefds*` | `excptfds*` | `timespec*` |

旧代码把 `pselect6` 当 poll 用，会把 `regs[0]`（整数 `nfds`）当 `pollfd*`
解引用 —— 野指针，幸运时崩，不幸时返回垃圾。

## 修复

文件：[`kernel/syscall/syscall.c`](../kernel/syscall/syscall.c)

### 1. 新增 UART ring buffer 空查询

```c
static inline int uart_ringbuf_empty(void) {
    return g_uart_rb_tail == g_uart_rb_head;
}
```

### 2. 重写 `poll` / `ppoll`

- 按 UART 真实状态报告 stdin (fd=0) 的 POLLIN
- 其他 fd（管道/普通文件等）继续视为 POLLIN|POLLOUT 就绪
- 正确解析 timeout：
  - `poll`：`regs[2]` 是 `int timeout_ms`（`-1` 阻塞 / `0` 立即返回 / `>0` 毫秒）
  - `ppoll`：`regs[2]` 是 `struct timespec*`（`NULL` 阻塞 / `{0,0}` 立即返回）
- 阻塞循环里调 `signal_check_uart()` + `task_yield()`，并响应信号返回 `-EINTR`
- 用 `kernel_get_ns()` 计算 deadline

### 3. 重写 `pselect6` 并新增 `select`（x86_64 syscall 23）

- 按 `fd_set` 位图 ABI 正确解析（每位一个 fd）
- 备份输入 `fd_set`，循环重新检查 readiness，避免多轮丢位
- `select` 用 `struct timeval`，`pselect6` 用 `struct timespec`
- 同样支持阻塞/超时/`-EINTR`
- 新增 syscall 号定义与 x86 翻译：

  ```c
  #define X86_SYS_SELECT  0x7FFFFFFAULL
  case 23: *nr = X86_SYS_SELECT; break; /* select */
  ```

### 4. busybox applet 链接列表补全

文件：[`install-apps.sh`](../install-apps.sh)

```bash
for applet in sh ls cat echo pwd mkdir rm cp mv grep find ps kill \
              vi more less head tail wc sleep date stty clear; do
    cp "$BUSYBOX_SRC" "$STAGE_DIR/bin/$applet"
    chmod +x "$STAGE_DIR/bin/$applet"
done
```

之前 `bin/vi` 不存在，用户得手敲 `busybox vi …` 才能起来。

## 验证

```bash
make clean
make ARCH=x86_64 LOG=warn -j8
make PLATFORM=qemu-virt-x86_64 run-fs
```

进入 shell 后：

```sh
vi /etc/hostname
```

- `i` 进入插入模式、敲字符回显正常
- `ESC` 立即回到 normal mode（不再卡）
- `hjkl` 光标四向移动
- `:wq` / `:q!` 正常退出

退出 QEMU：`Ctrl-A x`

aarch64 / riscv64 同步编译通过（共用同一份 syscall 调度器）。

## 经验教训

- `poll`/`select` 的 readiness 语义必须真实——“总是 ready”看似无害，
  对依赖超时的应用（vi 的 ESC 检测、shell 的 SIGINT 节流、各种事件循环）
  会破坏行为而不是仅仅“低效”。
- `poll`/`ppoll`/`pselect6`/`select` 四个 syscall 的参数布局两两不同，
  合并 case 时必须按 syscall 号判定 ABI；不能用同一段代码统吃。
- 实现 syscall 时如果暂时只支持“非阻塞返回”，**也要在循环里 yield**，
  否则单任务系统下 UART 中断不会有窗口进来。
- busybox 通过 `argv[0]` 选 applet；rootfs 里必须为每个想用的命令建好
  软链/拷贝，否则连入口都没有。

---
**修复日期**：2026-05-17
**涉及文件**：[`kernel/syscall/syscall.c`](../kernel/syscall/syscall.c)、[`install-apps.sh`](../install-apps.sh)
