# Avatar OS 测试框架

统一入口在 **`tests/Makefile`**（根 Makefile 不再继续膨胀）。

## 快速开始

```bash
make -C tests help

# 首次：拉 LTP 源码 → tests/ltp/src（434MB，不入库；ltp-build 会自动先跑）
make env

# 快速门禁：三架构构建 + 启动冒烟 + 内核自测 + apps/c 用户态测试
make -C tests check

# SMP=4 矩阵（独立 BUILD_ROOT；check + racetest）
make -C tests check-smp

# 只跑 SMP 竞争压力（含已知的 riscv64 间歇性问题）
make -C tests racetest SMP=4

# 内存重构专用门禁：再加 LTP mm 子集 + VMM guest 直启/helper
make -C tests check-mm
make -C tests check-mm-smp     # 同上，SMP=4

# 全量（含 LTP full；耗时很长）
make -C tests check-full
```

常用变量：

```bash
ARCHES='aarch64 riscv64 x86_64'
SMP=1                 # 构建期 CONFIG_SMP_CPUS，同时也是 QEMU -smp
SMP4=4                # check-smp / check-mm-smp 用的 SMP 值
LOG=info              # 默认 info；boot/helper 依赖 INFO 标记，别降成 warn
TIMEOUT_SCALE=2.0     # TCG 慢机器/机器有负载时调大
AVATAR_TEST_NO_KVM=1  # 强制 TCG；x86 VMM/guest 会标 SKIP
```

## 目录

```
tests/
├── Makefile              # 入口
├── run.py                # 统一 runner
├── framework/
│   ├── platforms.py      # 三架构构建/QEMU 命令、BUILD_ROOT 隔离
│   ├── qemu.py           # PTY 驱动、探针等待、禁止模式
│   ├── context.py        # 构建缓存、用例包装
│   ├── ltp.py            # LTP 逐例解析 + 失败基线
│   └── report.py         # JSON / Markdown / 控制台
├── suites/
│   ├── build.py          # 构建矩阵
│   ├── boot.py           # 启动冒烟
│   ├── kernel.py         # PMM / string / VMM_TEST
│   ├── ltp.py            # LTP
│   └── vmm_guest.py      # 直启 / helper / 多 VM
├── ltp/
│   ├── fetch.sh          # make env：拉 LTP 源码到 src/
│   ├── build.sh          # 交叉编译（需要 src/）
│   ├── run_ltp.sh        # 新增 LTP-CASE 机器可读行
│   ├── smoke.list        # PR 门禁子集
│   ├── mm.list           # 内存重构子集
│   ├── expected_failures.txt
│   └── src/              # LTP 源码（make env 拉取；gitignored）
└── .artifacts/           # 构建产物 + 运行日志（gitignored）
```

## 产物隔离（重要）

根 Makefile 的 `BUILD_ROOT` 在命令行可覆盖，测试框架给每个 **变体 + SMP**
一个独立目录：

```
tests/.artifacts/build/<variant>/smp<N>/<platform>/
    kernel_<arch>.bin
    rootfs-<arch>.img
```

因此 `GUEST_LINUX=1` / 普通 / `VMM_TEST` / `PMM_TEST` 之间**不会互相覆盖**。
注意：`PMM_TEST` 是本框架给根 Makefile 新增的正式变体（`RUN_PMM_TESTS`）。

运行日志：

```
tests/.artifacts/runs/<run-id>/
    report.json / report.md
    logs/<suite>/<arch>-<name>.log
```

失败时保留原始串口日志；成功也保留（磁盘不够时手动 `make -C tests clean`）。

## Suite 说明

| suite | 内容 | 判据 |
|---|---|---|
| `build` | 三架构 × normal/guest/vmm-test/string-test/pmm-test | 构建成功；guest 产物带 `GUEST_LINUX_BUILD_TAG` |
| `boot` | 三架构不带 rootfs 启动 | 出现 `PMM: N reserved regions`；无 ASSERT/panic/invalid free |
| `kernel` | `pmm-test` / `string-test` / `vmm-test` | 各自的 PASS 标记；三路线程标记齐全 |
| `apps` | `apps/c`：pthread / mutex | `[PASS]`/`[FAIL]` 行 |
| `racetest` | `apps/c/racetest.c`：slot/pipe/wake | 需 `SMP>1`；要求输出 `DONE`；**诊断 suite** |
| `ltp-smoke` | `smoke.list` | 无新增 FAIL/BROK；子集每项都有结果 |
| `ltp-mm` | `mm.list` | 同上；内存重构专用 |
| `ltp-full` | `/ltp` 全量 | 同上 |
| `vmm-direct` | guest Linux 直启 | guest uname + 交互 `echo`/`/proc/meminfo` |
| `vmm-helper` | 宿主 shell → `/bin/vmm-run` | guest 交互 + `Ctrl+T k` + 再次启动 |
| `vmm-multivm` | 多 VM detach/attach | `tools/vmm_multivm_regress.sh` 返回 0 |

### LTP 的失败基线

`tests/ltp/expected_failures.txt` 只登记**已确认是兼容性差异/环境依赖、
按项目定位不修**的项。格式：

```
x86_64 ioctl02 BROK
*      somecase FAIL
```

新增失败会让 suite 变红；登记过的显示为 known，不算失败。不要把真 bug
塞进基线。

### SMP 矩阵

SMP 是**编译期**标志（`CONFIG_SMP_CPUS`），框架把它放进 BUILD_ROOT：

```
tests/.artifacts/build/<variant>/smp<N>/<platform>/
```

- `make -C tests check` / `check-mm`：SMP=1；
- `make -C tests check-smp` / `check-mm-smp`：SMP=4（`SMP4` 可改）；
- `check-smp` 额外跑 `racetest`（SMP 竞争压力，独立诊断 suite）；
- `check-mm` / `check-mm-smp` **不包含** racetest，避免已知的 riscv64
  调度器间歇性 bug 让内存重构门禁随机变红。

### x86 / KVM

x86_64 的 VMX / guest 路径必须要 KVM（TCG 不支持项目当前路径）。
没有 `/dev/kvm` 时：

- build / boot / kernel / LTP 仍会跑（QEMU 用 TCG）；
- `vmm-direct` / `vmm-helper` / `vmm-multivm` 标 **SKIP**，不是 FAIL。

### 不要并行跑 VMM 类测试

vCPU 任务钉在单核上，机器被压满时 guest 时钟会慢到看起来像卡死
（见 `tools/vmm_helper_regress.sh` 顶部）。框架默认串行跑所有 suite。
构建可以用 `JOBS=$(nproc)`。

## 新增测试

- **内核自测**：在 `tests/` 加 `foo_test.c`，按现有模式在 `kernel/main.c`
  的变体分支里调用；需要新变体就仿照根 Makefile 的 `PMM_TEST` 块加
  `FOO_TEST` 和 `_BUILD_VARIANT`。
- **apps/c**：新增用户态测试后先跑 `bash apps/c/build.sh` 生成动态 rootfs；
  `tests/suites/apps.py` 使用 `imgs/rootfs-<arch>.img`（pthread/mutex）和
  框架 normal rootfs（racetest）。
- **LTP**：先把测例加进 `tests/ltp/testcases.list`；源码由 `make env`
  拉到 `tests/ltp/src/`，然后 `make -C tests ltp-build`（会自动先跑 env；
  三架构都要重编，否则会静默用旧产物）。
- **新的 QEMU 场景**：在 `suites/` 里加一个 `run(ctx, report, arches)`，
  在 `run.py` 的 `SUITE_NAMES` 里登记，Makefile 加一个 target。

## 框架已经暴露的问题

- **导出的环境变量没有跨 execve 传递**：在宿主 shell 里
  `export LTP_TESTS=...; sh -c 'echo $LTP_TESTS'` 打印空。LTP 子集因此
  改用 `. /ltp/run_ltp.sh`（source，同 shell）而不是执行子进程。这是一个
  独立的内核 bug，值得单独立项（不在内存重构范围内）。
- **x86 串口输入会丢字节**：一次性写 100+ 字节的命令行时实测丢字符
  （`echo` → `cho`、`__AVATAR` 少一个下划线）。框架对交互命令统一用
  `Qemu.send_slow()` 分块慢速写。
- **VMM_TEST 的 aarch64 guest 代码需要显式写进 guest RAM**：per-VM
  stage-2 是空表 + 按需分页，不再 identity map。`tests/unit-test/vmm_test.c` 已
  改用 `guest_loader_write_guest()` 预拷贝，否则 guest 从全 0 页取指 →
  VBAR=0 → VMM 退出。
- **x86 直启模式的 guest 输入在 SMP=4 下会丢字节**（vpl011 输入路径）。
  因此 `vmm-direct` 只验证"boot 到 guest shell"，**交互式输入由
  `vmm-helper` 负责**（三架构 SMP=1/4 均已通过）。
- **riscv64 SMP=4 的 `racetest wake` 偶发内核异常或挂死**：实测一次
  `[exception] sync: code=7 pc=sched_schedule+0x15a stval=0x8`
  （回溯 `net_poll_task`），另一次直接挂到 240s 超时；重复跑 3 次又全过。
  这是已知的 SMP 调度/运行队列间歇性问题，`racetest` 是独立诊断 suite，
  不在稳定的 `check-mm` / `check-mm-smp` 里；`make -C tests check-smp`
  会如实变红。
- **apps/c 的 mutex_test 计数器**：原 Test 5 用普通 `++` 统计跨线程
  trylock 结果，SMP>1 下计数器自身会丢更新（`acquired+failed != total`）。
  已改成 `__atomic_fetch_add`；改完必须把新二进制装进
  `imgs/rootfs-*.img`。`apps/c/build.sh` 现在会检查 staging 里有没有
  `busybox`，没有就拒绝重建 —— 否则会把完整动态 rootfs 覆盖成只有
  `/bin/{pthread,mutex}_test` 的 4.5M 镜像，宿主 shell 都起不来。

## CI

`.github/workflows/tests.yml` 有两档：

| job | runner | 内容 |
|---|---|---|
| `hosted-smoke` | `ubuntu-latest` | `make -C tests check`（build/boot/kernel/apps）。无 KVM，x86 VMM/guest 自动 SKIP；不跑 LTP/VMM guest |
| `self-hosted-full` | `[self-hosted, linux, x64]` | `check-mm` SMP=1/4（含 LTP mm + 三架构 VMM guest）、`racetest`（诊断，允许已知 flake）、nightly LTP full |

启用 full job：

1. 注册带 `self-hosted, linux, x64` 标签的 runner，确保有
   `/dev/kvm`、`aarch64/riscv64/x86_64-linux-musl-gcc`、QEMU、e2fsprogs；
2. 仓库 Settings → Variables 增加 `AVATAR_SELF_HOSTED=true`；
3. （可选）把 `tests/ltp/src` 源码和 `tests/ltp/bin/<arch>` 留在 runner
   工作区；workflow 用 `actions/checkout` 的 `clean: false` 保留它们，
   并在需要时执行 `make env` 拉取源码。没有 LTP 时 LTP suite 会 SKIP，
   不会误报通过。

`hosted-smoke` 会自动下载 musl 工具链并缓存到 `~/.avatar-toolchains`；
`self-hosted-full` 的 `tools/ci_selfhosted_env.sh` 会检查工具链/KVM 并把
常见本地工具链目录（`Desktop/Software/compiler/*/bin` 等）加入 PATH。

## 与旧脚本的关系

`tools/boot_regress.sh`、`tools/vmm_helper_regress.sh`、
`tools/vmm_multivm_regress.sh`、`tests/ltp/run_qemu_ltp.py` 仍然保留；
新框架优先复用它们已经验证过的等待/判定手法。`vmm_multivm_regress.sh`
和 `vmm_helper_regress.sh` 已支持 `KERNEL_BIN` / `ROOTFS_IMG` 环境变量，
框架会用独立产物路径传入，不再依赖 `build/<platform>` 的全局路径。
