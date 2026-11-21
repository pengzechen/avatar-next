# apps — C API

[← 返回总览](index.md) · 148 个函数 · 说明覆盖率 100.0%

<a id="mod-apps-c-dmatest.c"></a>

## apps/c/dmatest.c — dma-buf / dma-heap(P0)的用户态验收程序

`apps/c/dmatest.c` · 4 个函数 · 4 个有说明 · 319 行

跑法：
make PLATFORM=qemu-virt-aarch64 DMA=1 rootfs
make PLATFORM=qemu-virt-aarch64 DMA=1 run-fs
~ # /bin/dmatest

验收目标(对应 docs/npu/RK3588_NPU_DRIVER.md §5 的 P0)：
open(/dev/dma_heap/...) → DMA_HEAP_IOCTL_ALLOC → mmap → 读写一致,
并且反复分配/关闭不泄漏(fd 池和缓冲区表都只有 128 个槽,
漏一个跑 128 轮就会现形)。

注意： 本程序不碰 NPU, 所以它能证明的只有"管路通"：
缓存维护那两条(DMA_BUF_IOCTL_SYNC)只会被执行, 不会产生可观测差异
—— 同一个 CPU 自己写的自己读, cache 天然一致。真正的正确性要等
P3 有设备 DMA 进来才验得了。这里只能确认"ioctl 返回 0, 不报错"。

注意： 下面的结构体与 ioctl 号是手抄的(用户态不能包含内核头)。
_IOC 把 sizeof 编进了操作码, 所以尺寸写错不是"参数错位"而是换成
另一个 ioctl 号 —— 内核那边会走到 default 分支返回 -ENOSYS,
是响亮的失败, 不是静默损坏。内核侧同名定义见 include/driver/dma_buf.h。

<a id="fn-apps-c-dmatest.c-65"></a>

### `ok`

```c
static void ok(const char *what)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

打印 [ok] 与测试项名称，并把全局通过计数 g_pass 加一。

**位置**：`apps/c/dmatest.c:65`

<a id="fn-apps-c-dmatest.c-71"></a>

### `bad`

```c
static void bad(const char *what, const char *why)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

打印 [FAIL]、测试项名称与失败原因，并把全局失败计数 g_fail 加一。

**位置**：`apps/c/dmatest.c:71`

<a id="fn-apps-c-dmatest.c-78"></a>

### `roundtrip`

```c
static int roundtrip(int heap_fd, size_t len, uint8_t seed, const char *tag)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

分配 dma-heap 缓冲并 mmap，写入 seed 花纹后做两次 SYNC 再读回校验，最后释放；失败累加 g_fail 并返回 -1。

<details>
<summary>函数内备注（4 条）</summary>

**第 105 行**

> 写一个可校验的花纹(seed 让不同缓冲区互不相同)

**第 109 行**

> CPU 写完了要交给&quot;设备&quot;看 → END|WRITE(clean)

**第 117 行**

> &quot;设备&quot;写完了 CPU 要读 → START|READ(invalidate), 再读回来校验

**第 138 行（行尾）**

> 关闭应当把缓冲区还给堆

</details>

**位置**：`apps/c/dmatest.c:78`

<a id="fn-apps-c-dmatest.c-142"></a>

### `main`

```c
int main(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

依次测试 dma-heap 打开、多尺寸往返、8 个缓冲区互不覆盖、300 轮泄漏、越界 mmap 被拒与 fork 共享语义，按 g_fail 返回。

<details>
<summary>函数内备注（9 条）</summary>

**第 146 行**

> ── 1. 打开堆 ───────────────────────────────────────────────────

**第 164 行**

> ── 2. 各种尺寸的往返 ───────────────────────────────────────────

**第 170 行**

> ── 3. 多个缓冲区必须互不重叠 ───────────────────────────────────

**第 222 行**

> ── 4. 反复分配/关闭不泄漏 ───────────────────────────────────────

**第 225 行**

> fd 池和 dma_buf 表都只有 128 个槽。若 close 路径漏还引用
> (fd_obj_close 没走到 dma_buf_put_index), 这里 128 轮内必然失败。

**第 242 行**

> ── 5. 越界 mmap 必须被拒 ───────────────────────────────────────

**第 263 行**

> ── 6. fork 之后仍然是同一块物理内存 ───────────────────────────

**第 278 行**

> 打的是 PTE_KEEP 的 clone 分支：认不出这一位的实现会把设备
> 缓冲当成普通私有页复制一份给子进程 —— 内存是安全的,
> 但父子从此看着两块不同的内存。这种错不崩, 只让结果对不上。

**第 286 行**

> 子进程：必须看到父进程写的值(同一块内存, 不是副本)

</details>

**位置**：`apps/c/dmatest.c:142`

<a id="mod-apps-c-drmtest.c"></a>

## apps/c/drmtest.c — DRM 渲染节点 + GEM 层(P2+P4)的用户态验收

`apps/c/drmtest.c` · 3 个函数 · 3 个有说明 · 401 行

跑法：
make PLATFORM=qemu-virt-aarch64 LOG=info NPU=rknpu rootfs
make PLATFORM=qemu-virt-aarch64 LOG=info NPU=rknpu run-fs
~ # /bin/drmtest

注意： QEMU 上没有 NPU, 所以只有 SUBMIT 会失败(-ENODEV, 这是预期的)。
其余全部可验 —— 因为 GEM/PRIME/MEM_* 这些根本不碰硬件, 它们只是把
dma-buf 包装成 handle。所以这个程序能证明「librknnrt 连上来之后的
内存路径是通的」, 证明不了「算子算得对」(那要真机 + P3)。

注意： 下面的结构体与 ioctl 号是手抄的(用户态不能包含内核头)。
_IOC 把 sizeof 编进操作码, 所以尺寸写错不是"参数错位"而是换成另一个
ioctl 号, 内核会走到 default 分支回 -EINVAL —— 是响亮的失败。
内核侧同名定义见 include/npu/rknpu_drm.h, 两边靠这里的 _Static_assert 对齐。

<a id="fn-apps-c-drmtest.c-142"></a>

### `ok`

```c
static void ok(const char *w)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

打印 [ok] 与测试项名称，并把全局通过计数 g_pass 加一。

**位置**：`apps/c/drmtest.c:142`

<a id="fn-apps-c-drmtest.c-147"></a>

### `bad`

```c
static void bad(const char *w, const char *why)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

打印 [FAIL]、测试项名称与失败原因，并把全局失败计数 g_fail 加一。

**位置**：`apps/c/drmtest.c:147`

<a id="fn-apps-c-drmtest.c-153"></a>

### `main`

```c
int main(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

打开 renderD128 后逐项检查 DRM 核心与 RKNPU ioctl、PRIME 往返和 GEM_FLINK 拒绝，用 ok/bad 累计结果。

<details>
<summary>函数内备注（19 条）</summary>

**第 157 行**

> ── 1. 打开渲染节点 ─────────────────────────────────────────────

**第 166 行**

> ── 2. DRM 核心 ioctl ───────────────────────────────────────────

**第 183 行**

> 注意： 名字必须是 rknpu —— librknnrt 按它做身份匹配。
> (最初起的是 &quot;rknpu-avatar&quot;, librknnrt 认不出, 空指针崩。)

**第 198 行**

> librknnrt 会发这两个做探测；渲染节点上 setmaster 语义上就该成功

**第 204 行**

> AGP 是更老的东西, 我们确实没有 —— 明确 -EINVAL 才是对的

**第 211 行**

> ── 3. ACTION：IOMMU 查询(librknnrt 的决策依据)────────────────

**第 221 行**

> ── 4. dma-buf → GEM handle 往返 ────────────────────────────────

**第 235 行**

> 从 dmabuf fd 换 handle

**第 245 行**

> 再换回一个 fd, 应当指向同一块内存

**第 253 行**

> 两个 fd 映射到同一块内存才说明往返没走丢

**第 270 行**

> GEM_FLINK 我们明确不支持, 应当 -EINVAL 而不是假成功

**第 277 行**

> handle 要能被销毁；销毁后再用应当失败

**第 290 行**

> ── 5. 驱动自带的 MEM_CREATE / MEM_MAP 路径 ─────────────────────

**第 308 行**

> 非连续缓冲必须被拒(无 IOMMU)

**第 312 行（行尾）**

> RKNPU_MEM_NON_CONTIGUOUS

**第 318 行**

> MEM_MAP 回假偏移 → mmap 渲染节点

**第 358 行**

> ── 6. SUBMIT：QEMU 上没有 NPU, 预期 -ENODEV ────────────────────

**第 388 行**

> ── 7. 未实现的 ioctl 应当被响亮拒绝(不是静默)─────────────────

**第 391 行**

> 0xc0106418 是反汇编里还原出来但没定性的一个号

</details>

**位置**：`apps/c/drmtest.c:153`

<a id="mod-apps-c-epoll_perf.c"></a>

## apps/c/epoll_perf.c

`apps/c/epoll_perf.c` · 4 个函数 · 4 个有说明 · 112 行

<a id="fn-apps-c-epoll_perf.c-17"></a>

### `now_us`

```c
static uint64_t now_us(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

调用 gettimeofday 取当前时间，并换算成自 epoch 起的微秒数返回。

**位置**：`apps/c/epoll_perf.c:17`

<a id="fn-apps-c-epoll_perf.c-24"></a>

### `parse_int`

```c
static int parse_int(const char *s, int fallback)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 strtol 解析十进制，空串、带尾随字符、非正数或超过 1 亿时返回传入的默认值。

**位置**：`apps/c/epoll_perf.c:24`

<a id="fn-apps-c-epoll_perf.c-35"></a>

### `run_pipe_bench`

```c
static int run_pipe_bench(int iterations, int batch)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

建管道并注册到 epoll，循环写入 batch 字节、epoll_wait 等待、再读出，最后打印事件与字节吞吐及每轮耗时。

**位置**：`apps/c/epoll_perf.c:35`

<a id="fn-apps-c-epoll_perf.c-107"></a>

### `main`

```c
int main(int argc, char **argv)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从命令行取迭代数与批大小（缺省 10000 与 1），转调 run_pipe_bench 并返回其结果。

**位置**：`apps/c/epoll_perf.c:107`

<a id="mod-apps-c-mutex-test.c"></a>

## apps/c/mutex/test.c — 基于 futex 的用户态 mutex 实现与测试

`apps/c/mutex/test.c` · 21 个函数 · 21 个有说明 · 487 行

展示如何用内核 futex 系统调用(FUTEX_WAIT / FUTEX_WAKE)从零实现 mutex,
这正是 pthread_mutex 的底层机制。

实现了两种 mutex：
umutex_t    — 不可重入的高效 futex mutex(三态：0=空闲 1=锁定无等待者 2=锁定有等待者)
rmutex_t    — 基于 umutex_t 的可重入(递归)mutex

测试项：
Test 0  无锁竞争演示(对照组, 证明竞争确实发生)
Test 1  umutex 基础单线程 lock / unlock / trylock
Test 2  umutex 多线程计数器(等价于 pthread Test 2)
Test 3  rmutex 单线程递归加锁(lock 3 次 → unlock 3 次)
Test 4  rmutex 多线程 + 递归深度 2
Test 5  umutex trylock 竞争场景

<a id="fn-apps-c-mutex-test.c-53"></a>

### `futex`

```c
static inline long futex(atomic_int *addr, int op, int val)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把给定操作与期望值封装成 SYS_futex 系统调用并返回其结果。

**位置**：`apps/c/mutex/test.c:53`

<a id="fn-apps-c-mutex-test.c-73"></a>

### `umutex_init`

```c
static void umutex_init(umutex_t *m)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把用户态互斥锁的 state 原子初始化为 0。

**位置**：`apps/c/mutex/test.c:73`

<a id="fn-apps-c-mutex-test.c-78"></a>

### `umutex_lock`

```c
static void umutex_lock(umutex_t *m)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

先 CAS 0→1 走快路径，竞争时把 state 设为 2 并 FUTEX_WAIT 睡眠，直到成功获得锁。

<details>
<summary>函数内备注（5 条）</summary>

**第 82 行**

> 快速路径：0 → 1(无竞争)

**第 87 行**

> 慢速路径：有竞争, 设置 state=2 后进入 futex 睡眠

**第 89 行**

> 若当前 state=1 或 state=2, 先确保 state=2(通知 unlock 要唤醒)

**第 92 行**

> 睡眠直到 state != 2

**第 95 行**

> 尝试以 state=2 方式获取(保留&quot;有等待者&quot;信息)

</details>

**位置**：`apps/c/mutex/test.c:78`

<a id="fn-apps-c-mutex-test.c-100"></a>

### `umutex_trylock`

```c
static int umutex_trylock(umutex_t *m)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

原子比较 state 是否为 0 并置 1，成功返回非零、失败返回 0。

**位置**：`apps/c/mutex/test.c:100`

<a id="fn-apps-c-mutex-test.c-106"></a>

### `umutex_unlock`

```c
static void umutex_unlock(umutex_t *m)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

对 state 做 fetch_sub，原值不为 1（有等待者）时清零并 FUTEX_WAKE 唤醒一个等待者。

<details>
<summary>函数内备注（1 条）</summary>

**第 108 行**

> fetch_sub: 1→0 无等待者直接返回；2→1 有等待者需要唤醒

</details>

**位置**：`apps/c/mutex/test.c:106`

<a id="fn-apps-c-mutex-test.c-131"></a>

### `rmutex_init`

```c
static void rmutex_init(rmutex_t *m)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

初始化内层 umutex，把 owner 与递归计数 count 都置 0。

**位置**：`apps/c/mutex/test.c:131`

<a id="fn-apps-c-mutex-test.c-138"></a>

### `rmutex_lock`

```c
static void rmutex_lock(rmutex_t *m)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

若当前线程已是持有者则只递增 count，否则先取底层锁再记录 owner 并把 count 置 1。

<details>
<summary>函数内备注（3 条）</summary>

**第 142 行**

> 重入检测：已是持有者, 直接递增计数

**第 148 行**

> 非持有者：获取底层锁

**第 150 行**

> 设置 owner(此时我们已独占锁, 安全写入)

</details>

**位置**：`apps/c/mutex/test.c:138`

<a id="fn-apps-c-mutex-test.c-155"></a>

### `rmutex_unlock`

```c
static void rmutex_unlock(rmutex_t *m)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

递减 count，仍大于 0 则直接返回，否则清 owner 并释放底层 umutex。

<details>
<summary>函数内备注（2 条）</summary>

**第 157 行**

> 递减计数；不为 0 则仍在递归持有中

**第 161 行**

> 清除 owner 后再释放底层锁

</details>

**位置**：`apps/c/mutex/test.c:155`

<a id="fn-apps-c-mutex-test.c-172"></a>

### `pass`

```c
static void pass(const char *name)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

打印带 [PASS] 前缀的测试名。

**位置**：`apps/c/mutex/test.c:172`

<a id="fn-apps-c-mutex-test.c-176"></a>

### `fail`

```c
static void fail(const char *name, const char *reason)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

打印带 [FAIL] 前缀的测试名与失败原因。

**位置**：`apps/c/mutex/test.c:176`

<a id="fn-apps-c-mutex-test.c-188"></a>

### `t0_worker`

```c
static void *t0_worker(void *arg)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

循环 RACE_LOOPS 次做非原子读-让出-写，故意制造丢失更新。

**位置**：`apps/c/mutex/test.c:188`

<a id="fn-apps-c-mutex-test.c-199"></a>

### `test0_race_demo`

```c
static void test0_race_demo(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

开多线程跑竞态自增并 join，比较计数与期望值，打印丢失量以确认竞态。

**位置**：`apps/c/mutex/test.c:199`

<a id="fn-apps-c-mutex-test.c-227"></a>

### `test1_umutex_basic`

```c
static void test1_umutex_basic(void)
```

═══════════════════════════════════════════════════════════════════
Test 1: umutex 基础单线程
═══════════════════════════════════════════════════════════════════

<details>
<summary>函数内备注（3 条）</summary>

**第 231 行**

> lock → unlock

**第 243 行**

> trylock(空闲 → 成功)

**第 248 行**

> trylock 已持有时(不可重入, 应失败)

</details>

**位置**：`apps/c/mutex/test.c:227`

<a id="fn-apps-c-mutex-test.c-268"></a>

### `t2_worker`

```c
static void *t2_worker(void *arg)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

循环 LOOP_COUNT 次在 umutex 保护下自增 t2_counter。

**位置**：`apps/c/mutex/test.c:268`

<a id="fn-apps-c-mutex-test.c-279"></a>

### `test2_umutex_counter`

```c
static void test2_umutex_counter(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

初始化 umutex 后起多线程各加 LOOP_COUNT 次，比较最终计数与期望并打印 pass/fail。

**位置**：`apps/c/mutex/test.c:279`

<a id="fn-apps-c-mutex-test.c-305"></a>

### `test3_rmutex_recursive`

```c
static void test3_rmutex_recursive(void)
```

═══════════════════════════════════════════════════════════════════
Test 3: rmutex 单线程递归加锁
═══════════════════════════════════════════════════════════════════

<details>
<summary>函数内备注（2 条）</summary>

**第 309 行**

> 递归加锁三次

**第 328 行**

> 对应三次解锁：前两次不释放锁

</details>

**位置**：`apps/c/mutex/test.c:305`

<a id="fn-apps-c-mutex-test.c-367"></a>

### `t4_worker`

```c
static void *t4_worker(void *arg)
```

每个线程加锁两次(递归深度 2), 递增计数器, 再解锁两次

<details>
<summary>函数内备注（4 条）</summary>

**第 371 行（行尾）**

> 第 1 次

**第 372 行（行尾）**

> 第 2 次(重入)

**第 374 行（行尾）**

> 释放第 2 次

**第 375 行（行尾）**

> 释放第 1 次(真正释放)

</details>

**位置**：`apps/c/mutex/test.c:367`

<a id="fn-apps-c-mutex-test.c-380"></a>

### `test4_rmutex_threaded`

```c
static void test4_rmutex_threaded(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

初始化递归锁后起多线程各自累加计数并 join，比较总数与期望值后打印 pass/fail。

**位置**：`apps/c/mutex/test.c:380`

<a id="fn-apps-c-mutex-test.c-410"></a>

### `t5_worker`

```c
static void *t5_worker(void *arg)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

循环 LOOP_COUNT 次尝试 trylock，成功则原子累加 acquired 并解锁，失败则原子累加 failed。

<details>
<summary>函数内备注（1 条）</summary>

**第 415 行**

> 计数器是跨线程共享的, 普通 ++ 在 SMP&gt;1 下会丢更新
> (实测 4 线程 × 20000 次时 acquired+failed &lt; total, 被
> 测试框架当成内核失败)。这里只要求最终计数准确, 用
> relaxed 原子加即可。

</details>

**位置**：`apps/c/mutex/test.c:410`

<a id="fn-apps-c-mutex-test.c-430"></a>

### `test5_trylock_race`

```c
static void test5_trylock_race(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

起多线程各做 LOOP_COUNT 次 trylock，检查成功数与失败数之和是否等于总尝试数。

<details>
<summary>函数内备注（1 条）</summary>

**第 445 行**

> 所有尝试之和 = 成功 + 失败

</details>

**位置**：`apps/c/mutex/test.c:430`

<a id="fn-apps-c-mutex-test.c-457"></a>

### `main`

```c
int main(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

依次运行竞态演示、umutex 基本与多线程、rmutex 递归与多线程、trylock 竞争测试，最后打印 Done 并返回 0。

**位置**：`apps/c/mutex/test.c:457`

<a id="mod-apps-c-pthread-test.c"></a>

## apps/c/pthread/test.c — Avatar OS pthread 综合测试

`apps/c/pthread/test.c` · 16 个函数 · 16 个有说明 · 354 行

测试项：
Test 1  基本线程创建与 join
Test 2  互斥锁并发计数
Test 3  条件变量生产者/消费者
Test 4  多线程同时竞争同一互斥锁(压力测试)
Test 5  线程 TLS(__thread 变量)

<a id="fn-apps-c-pthread-test.c-33"></a>

### `pass`

```c
static void pass(const char *name)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

打印带 [PASS] 前缀的测试名。

**位置**：`apps/c/pthread/test.c:33`

<a id="fn-apps-c-pthread-test.c-37"></a>

### `fail`

```c
static void fail(const char *name, const char *reason)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

打印带 [FAIL] 前缀的测试名与失败原因。

**位置**：`apps/c/pthread/test.c:37`

<a id="fn-apps-c-pthread-test.c-47"></a>

### `t1_worker`

```c
static void *t1_worker(void *arg)
```

Test 1: 基本 pthread_create / pthread_join

<details>
<summary>函数内备注（1 条）</summary>

**第 50 行**

> 每个线程返回自己的 id * 2

</details>

**位置**：`apps/c/pthread/test.c:47`

<a id="fn-apps-c-pthread-test.c-54"></a>

### `test1_basic_join`

```c
static void test1_basic_join(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

创建 NUM_THREADS 个线程并逐个 join，检查每个返回值是否为 id*2，全部正确则 pass、否则 fail。

**位置**：`apps/c/pthread/test.c:54`

<a id="fn-apps-c-pthread-test.c-89"></a>

### `t0_worker_racy`

```c
static void *t0_worker_racy(void *arg)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

循环 RACE_LOOPS 次做非原子的读-自增-写，中间调用 sched_yield 让出 CPU，故意制造竞态。

<details>
<summary>函数内备注（4 条）</summary>

**第 93 行**

> 显式三步 read-modify-write, 中间 sched_yield 强制切换 CPU

**第 94 行（行尾）**

> step 1: read

**第 95 行（行尾）**

> step 2: 让出 CPU, 保证另一线程运行

**第 96 行（行尾）**

> step 3: write 过时值, 覆盖别人的更新

</details>

**位置**：`apps/c/pthread/test.c:89`

<a id="fn-apps-c-pthread-test.c-101"></a>

### `test0_race_demo`

```c
static void test0_race_demo(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

开 NUM_THREADS 个竞态线程并 join，比较计数器与理论值，打印丢失量与百分比以确认竞态存在。

**位置**：`apps/c/pthread/test.c:101`

<a id="fn-apps-c-pthread-test.c-132"></a>

### `t2_worker`

```c
static void *t2_worker(void *arg)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

循环 LOOP_COUNT 次在 pthread_mutex 保护下自增 t2_counter。

**位置**：`apps/c/pthread/test.c:132`

<a id="fn-apps-c-pthread-test.c-143"></a>

### `test2_mutex_counter`

```c
static void test2_mutex_counter(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

起 NUM_THREADS 个线程各加 LOOP_COUNT 次，比较最终计数与期望值并打印 pass 或 fail。

**位置**：`apps/c/pthread/test.c:143`

<a id="fn-apps-c-pthread-test.c-176"></a>

### `t3_producer`

```c
static void *t3_producer(void *arg)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

循环 total 次在互斥锁下把序号写入环形队列（满则 cond_wait），完成后置 t3_done 并 broadcast 唤醒消费者。

<details>
<summary>函数内备注（1 条）</summary>

**第 181 行（行尾）**

> 队列满则等待

</details>

**位置**：`apps/c/pthread/test.c:176`

<a id="fn-apps-c-pthread-test.c-196"></a>

### `t3_consumer`

```c
static void *t3_consumer(void *arg)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

在互斥锁下等待队列非空，累加取出元素之和并 signal 生产者，队列空且已完成后退出。

**位置**：`apps/c/pthread/test.c:196`

<a id="fn-apps-c-pthread-test.c-217"></a>

### `test3_cond_producer_consumer`

```c
static void test3_cond_producer_consumer(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

起一个消费者与一个生产者（共 200 项），join 后比较累加和是否等于 20100 并打印 pass/fail。

<details>
<summary>函数内备注（1 条）</summary>

**第 229 行（行尾）**

> 1+2+...+200 = 20100

</details>

**位置**：`apps/c/pthread/test.c:217`

<a id="fn-apps-c-pthread-test.c-250"></a>

### `t4_worker`

```c
static void *t4_worker(void *arg)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

循环 T4_LOOPS 次在互斥锁临界区内读-加一-写 t4_counter。

<details>
<summary>函数内备注（1 条）</summary>

**第 256 行**

> 模拟临界区里的工作：读-修改-写

</details>

**位置**：`apps/c/pthread/test.c:250`

<a id="fn-apps-c-pthread-test.c-264"></a>

### `test4_mutex_stress`

```c
static void test4_mutex_stress(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

起 T4_THREADS 个线程各在锁内自增 T4_LOOPS 次，比较总数与期望并打印 pass/fail。

**位置**：`apps/c/pthread/test.c:264`

<a id="fn-apps-c-pthread-test.c-291"></a>

### `t5_worker`

```c
static void *t5_worker(void *arg)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把线程局部变量 tls_val 置为 id*100，忙等一阵后返回该值，用于验证 TLS 是否被串改。

<details>
<summary>函数内备注（2 条）</summary>

**第 295 行**

> 让调度器有机会切换

**第 297 行（行尾）**

> 如果 TLS 正确, 应返回 id*100

</details>

**位置**：`apps/c/pthread/test.c:291`

<a id="fn-apps-c-pthread-test.c-300"></a>

### `test5_tls`

```c
static void test5_tls(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

为每个线程传不同 id，join 后检查返回值是否等于 id*100，全部正确才 pass。

**位置**：`apps/c/pthread/test.c:300`

<a id="fn-apps-c-pthread-test.c-327"></a>

### `main`

```c
int main(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

依次运行竞态演示、create/join、互斥计数、条件变量、互斥压力与 TLS 五项测试并打印结果，恒返回 0。

**位置**：`apps/c/pthread/test.c:327`

<a id="mod-apps-c-pthread_test.c"></a>

## apps/c/pthread_test.c — Avatar OS pthread 综合测试

`apps/c/pthread_test.c` · 16 个函数 · 16 个有说明 · 354 行

测试项：
Test 1  基本线程创建与 join
Test 2  互斥锁并发计数
Test 3  条件变量生产者/消费者
Test 4  多线程同时竞争同一互斥锁(压力测试)
Test 5  线程 TLS(__thread 变量)

<a id="fn-apps-c-pthread_test.c-33"></a>

### `pass`

```c
static void pass(const char *name)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

在标准输出打印带 [PASS] 前缀的测试名。

**位置**：`apps/c/pthread_test.c:33`

<a id="fn-apps-c-pthread_test.c-37"></a>

### `fail`

```c
static void fail(const char *name, const char *reason)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

在标准输出打印带 [FAIL] 前缀的测试名与失败原因。

**位置**：`apps/c/pthread_test.c:37`

<a id="fn-apps-c-pthread_test.c-47"></a>

### `t1_worker`

```c
static void *t1_worker(void *arg)
```

Test 1: 基本 pthread_create / pthread_join

<details>
<summary>函数内备注（1 条）</summary>

**第 50 行**

> 每个线程返回自己的 id * 2

</details>

**位置**：`apps/c/pthread_test.c:47`

<a id="fn-apps-c-pthread_test.c-54"></a>

### `test1_basic_join`

```c
static void test1_basic_join(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

创建 NUM_THREADS 个线程并逐个 join，检查每个返回值是否为 id*2，全部正确则 pass、否则 fail。

**位置**：`apps/c/pthread_test.c:54`

<a id="fn-apps-c-pthread_test.c-89"></a>

### `t0_worker_racy`

```c
static void *t0_worker_racy(void *arg)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

循环 RACE_LOOPS 次做非原子的读-自增-写，中间调用 sched_yield 让出 CPU，故意制造竞态。

<details>
<summary>函数内备注（4 条）</summary>

**第 93 行**

> 显式三步 read-modify-write, 中间 sched_yield 强制切换 CPU

**第 94 行（行尾）**

> step 1: read

**第 95 行（行尾）**

> step 2: 让出 CPU, 保证另一线程运行

**第 96 行（行尾）**

> step 3: write 过时值, 覆盖别人的更新

</details>

**位置**：`apps/c/pthread_test.c:89`

<a id="fn-apps-c-pthread_test.c-101"></a>

### `test0_race_demo`

```c
static void test0_race_demo(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

开 NUM_THREADS 个竞态线程并 join，比较计数器与理论值，打印丢失量与百分比以确认竞态存在。

**位置**：`apps/c/pthread_test.c:101`

<a id="fn-apps-c-pthread_test.c-132"></a>

### `t2_worker`

```c
static void *t2_worker(void *arg)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

循环 LOOP_COUNT 次在 pthread_mutex 保护下自增 t2_counter。

**位置**：`apps/c/pthread_test.c:132`

<a id="fn-apps-c-pthread_test.c-143"></a>

### `test2_mutex_counter`

```c
static void test2_mutex_counter(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

起 NUM_THREADS 个线程各加 LOOP_COUNT 次，比较最终计数与期望值并打印 pass 或 fail。

**位置**：`apps/c/pthread_test.c:143`

<a id="fn-apps-c-pthread_test.c-176"></a>

### `t3_producer`

```c
static void *t3_producer(void *arg)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

循环 total 次在互斥锁下把序号写入环形队列（满则 cond_wait），完成后置 t3_done 并 broadcast 唤醒消费者。

<details>
<summary>函数内备注（1 条）</summary>

**第 181 行（行尾）**

> 队列满则等待

</details>

**位置**：`apps/c/pthread_test.c:176`

<a id="fn-apps-c-pthread_test.c-196"></a>

### `t3_consumer`

```c
static void *t3_consumer(void *arg)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

在互斥锁下等待队列非空，累加取出元素之和并 signal 生产者，队列空且已完成后退出。

**位置**：`apps/c/pthread_test.c:196`

<a id="fn-apps-c-pthread_test.c-217"></a>

### `test3_cond_producer_consumer`

```c
static void test3_cond_producer_consumer(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

起一个消费者与一个生产者（共 200 项），join 后比较累加和是否等于 20100 并打印 pass/fail。

<details>
<summary>函数内备注（1 条）</summary>

**第 229 行（行尾）**

> 1+2+...+200 = 20100

</details>

**位置**：`apps/c/pthread_test.c:217`

<a id="fn-apps-c-pthread_test.c-250"></a>

### `t4_worker`

```c
static void *t4_worker(void *arg)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

循环 T4_LOOPS 次在互斥锁临界区内读-加一-写 t4_counter。

<details>
<summary>函数内备注（1 条）</summary>

**第 256 行**

> 模拟临界区里的工作：读-修改-写

</details>

**位置**：`apps/c/pthread_test.c:250`

<a id="fn-apps-c-pthread_test.c-264"></a>

### `test4_mutex_stress`

```c
static void test4_mutex_stress(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

起 T4_THREADS 个线程各在锁内自增 T4_LOOPS 次，比较总数与期望并打印 pass/fail。

**位置**：`apps/c/pthread_test.c:264`

<a id="fn-apps-c-pthread_test.c-291"></a>

### `t5_worker`

```c
static void *t5_worker(void *arg)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把线程局部变量 tls_val 置为 id*100，忙等一阵后返回该值，用于验证 TLS 是否被串改。

<details>
<summary>函数内备注（2 条）</summary>

**第 295 行**

> 让调度器有机会切换

**第 297 行（行尾）**

> 如果 TLS 正确, 应返回 id*100

</details>

**位置**：`apps/c/pthread_test.c:291`

<a id="fn-apps-c-pthread_test.c-300"></a>

### `test5_tls`

```c
static void test5_tls(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

为每个线程传不同 id，join 后检查返回值是否等于 id*100，全部正确才 pass。

**位置**：`apps/c/pthread_test.c:300`

<a id="fn-apps-c-pthread_test.c-327"></a>

### `main`

```c
int main(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

依次运行竞态演示、create/join、互斥计数、条件变量、互斥压力与 TLS 五项测试并打印结果，恒返回 0。

**位置**：`apps/c/pthread_test.c:327`

<a id="mod-apps-c-racetest.c"></a>

## apps/c/racetest.c — 并发竞争态复现工具(用户态, 静态链接)

`apps/c/racetest.c` · 9 个函数 · 9 个有说明 · 362 行

用途：把内核里几处「已定位但没复现」的时序竞争打成可观察的失败。
三个子命令各打一条, 都需要 SMP>1(单核下用户进程的内核侧
不会被抢占, 见 kernel/task/sched.c 的 trap_preempt_kernel_side)。

racetest slot <procs> <iters>
多个进程各自循环 fork+wait。打的是 fork 路径里内联的
任务槽「回收 → 认领」循环(kernel/syscall/core/proc_lifecycle.c),
它无锁：两个核可能同时 reap 同一个 DEAD 任务
(→ vm_destroy_user_process 双重释放页表 / PMM 双重 free),
或同时认领同一个空槽(→ 两个任务共用一块 TCB 和一块栈)。

失败特征：内核打印 "PMM: invalid free address",
"[sched] CORRUPT runqueue", 或直接 panic / 挂死。

racetest pipe <iters>
反复建管道, reader 阻塞读, writer 立刻写。打的是
kernel/syscall/fs/pipe.c 的丢唤醒：reader 在「读到 count==0」
与「写 p->blocked_reader」之间被另一核的 writer 插进来
(writer 写完数据, 检查 blocked_reader 还是 NULL),
reader 随后睡死, 虽然环里躺着数据。

失败特征：进度停在某一轮不再前进(本程序会每轮打印心跳)。

racetest wake <iters>
一个管道上放一个阻塞的 writer + 两个并发 reader。打的是
kernel/task/task.c 的 task_unblock 无锁：两个 reader 同时
看到 p->blocked_writer 非空, 同时调 task_unblock,
两边都在对方写 state 之前读到 TASK_BLOCKED → 两次
sched_enqueue 同一个节点 → 运行队列双向链表损坏。

失败特征："[sched] CORRUPT runqueue", 野指针崩溃, 挂死。

输出约定：每完成一轮打一个点, 便于把「挂着不动」和「还在跑」区分开；
全部跑完打 "DONE"。

<a id="fn-apps-c-racetest.c-60"></a>

### `parse_int`

```c
static int parse_int(const char *s, int fallback)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 strtol 解析十进制，空串、带尾随字符、非正数或超过 1 亿时返回传入的默认值。

**位置**：`apps/c/racetest.c:60`

<a id="fn-apps-c-racetest.c-73"></a>

### `slot_worker`

```c
static void slot_worker(int iters, int tag)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

循环 iters 次 fork 后由子进程立即退出、父进程 waitpid 回收，并统计打印 fork/waitpid 失败次数。

<details>
<summary>函数内备注（1 条）</summary>

**第 80 行**

> 子进程立刻退出 —— 目的就是制造 DEAD 槽给父进程回收

</details>

**位置**：`apps/c/racetest.c:73`

<a id="fn-apps-c-racetest.c-101"></a>

### `cmd_slot`

```c
static int cmd_slot(int nprocs, int iters)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

最多 fork 64 个 worker 各自跑 slot_worker，再逐个 waitpid 回收，最后打印 DONE。

**位置**：`apps/c/racetest.c:101`

<a id="fn-apps-c-racetest.c-138"></a>

### `cmd_seq`

```c
static int cmd_seq(int iters)
```

── seq：单进程「先 fork 两个, 再一起 wait」─────────────────────────

这一条不需要并发。fork 路径里的回收循环只判 state == TASK_DEAD,
不判 parent_id —— 建第二个子进程时会把第一个子进程(若已退出)的僵尸
顺手收掉, 于是后面 waitpid(first) 拿到 ECHILD。
shell 的 `a | b` 就是这个形态：两个子进程都 fork 完才 wait。

<details>
<summary>函数内备注（2 条）</summary>

**第 147 行（行尾）**

> 立刻死, 制造僵尸

**第 149 行**

> 不等 a, 直接 fork b —— 这一步的回收循环可能把 a 收走

</details>

**位置**：`apps/c/racetest.c:138`

<a id="fn-apps-c-racetest.c-178"></a>

### `cmd_seqw`

```c
static int cmd_seqw(int iters)
```

── seqw：对照组 —— fork 一个, 立刻 wait 一个 ──────────────────────

和 seq 的唯一区别是不重叠：任何时候最多只有一个子进程活着。
如果这条干净而 seq 崩, 就说明问题出在「建新子进程时顺手回收别人的
僵尸」这条路径上。

**位置**：`apps/c/racetest.c:178`

<a id="fn-apps-c-racetest.c-206"></a>

### `cmd_pipe`

```c
static int cmd_pipe(int iters)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

循环 iters 次建管道并 fork 一个阻塞读的读者，父进程立即写入一字节再 wait，每 50 轮打点。

<details>
<summary>函数内备注（2 条）</summary>

**第 221 行**

> reader：立刻阻塞读一个字节

**第 230 行**

> writer 抢在 reader 发布 blocked_reader 之前把数据写进去

</details>

**位置**：`apps/c/racetest.c:206`

<a id="fn-apps-c-racetest.c-252"></a>

### `cmd_wake`

```c
static int cmd_wake(int iters)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

循环 iters 次灌满管道、再 fork 一个阻塞写者与两个并发读 1 字节的读者，wait 全部子进程以打双唤醒。

<details>
<summary>函数内备注（3 条）</summary>

**第 265 行**

> 灌满到刚好满(管道容量就是 PIPE_BUF_SIZE)。
> 多写一个字节这里就会先阻塞住, 测例自己挂 —— 之前就是这么错的。

**第 275 行**

> writer：再写一个满 buf, 管道已满 → 阻塞在 p-&gt;blocked_writer

**第 283 行**

> 两个 reader 并发读 1 字节：读完之后都会尝试唤醒同一个
> blocked_writer。两边如果同时看到非 NULL, 就是双 unblock。

</details>

**位置**：`apps/c/racetest.c:252`

<a id="fn-apps-c-racetest.c-319"></a>

### `usage`

```c
static void usage(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

打印 racetest 支持的 slot/seq/seqw/pipe/wake 五个子命令及参数用法。

**位置**：`apps/c/racetest.c:319`

<a id="fn-apps-c-racetest.c-329"></a>

### `main`

```c
int main(int argc, char **argv)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按 argv[1] 分发到 slot/seq/seqw/pipe/wake 子命令（各自不同默认迭代数），未识别时打印用法并返回 2。

**位置**：`apps/c/racetest.c:329`

<a id="mod-apps-c-rt_demo_user.c"></a>

## apps/c/rt_demo_user.c — 用户态 RT 演示（/bin/rt-demo）

`apps/c/rt_demo_user.c` · 14 个函数 · 14 个有说明 · 312 行

演示现有 RT 设施的四件事：
1. event 模板：supervisor post 事件，RT worker 在被唤醒后执行 step；
测量 post→out_seq 可见的延迟（包含 syscall 与调度）。
2. 周期 worker：优先尝试 1ms（x86 oneshot），不支持则回退 10ms；
通过共享页写 input，统计 ticks/misses/exec_max/dev_writes。
3. watchdog 模板：停掉被监控的周期 worker 后，输出切到安全值。
4. /proc/rtworkers 快照（EVENTS/DROPS 列）。

运行：/bin/rt-demo（root）。结束输出 "=== Done ===" 供测试框架解析。

<a id="fn-apps-c-rt_demo_user.c-35"></a>

### `pass`

```c
static void pass(const char *what)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

在标准输出打印带 [PASS] 前缀的测试项。

**位置**：`apps/c/rt_demo_user.c:35`

<a id="fn-apps-c-rt_demo_user.c-40"></a>

### `fail`

```c
static void fail(const char *what)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

在标准输出打印带 [FAIL] 前缀的测试项，并把全局失败标志 g_fail 置 1。

**位置**：`apps/c/rt_demo_user.c:40`

<a id="fn-apps-c-rt_demo_user.c-46"></a>

### `now_ns`

```c
static uint64_t now_ns(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 clock_gettime(CLOCK_MONOTONIC) 取时间并换算成纳秒返回。

**位置**：`apps/c/rt_demo_user.c:46`

<a id="fn-apps-c-rt_demo_user.c-53"></a>

### `rt_stats`

```c
static int rt_stats(int fd, uint32_t id, struct rtw_stats *st)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

先清零统计结构并填入 id，再用 RTW_IOC_STATS ioctl 读取并返回结果。

**位置**：`apps/c/rt_demo_user.c:53`

<a id="fn-apps-c-rt_demo_user.c-60"></a>

### `rt_start`

```c
static int rt_start(int fd, uint32_t id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

以 id 构造 rtw_id_req 并下发 RTW_IOC_START ioctl，返回其结果。

**位置**：`apps/c/rt_demo_user.c:60`

<a id="fn-apps-c-rt_demo_user.c-66"></a>

### `rt_stop`

```c
static int rt_stop(int fd, uint32_t id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

以 id 构造 rtw_id_req 并下发 RTW_IOC_STOP ioctl，返回其结果。

**位置**：`apps/c/rt_demo_user.c:66`

<a id="fn-apps-c-rt_demo_user.c-72"></a>

### `rt_destroy`

```c
static int rt_destroy(int fd, uint32_t id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

以 id 构造 rtw_id_req 并下发 RTW_IOC_DESTROY ioctl，返回其结果。

**位置**：`apps/c/rt_demo_user.c:72`

<a id="fn-apps-c-rt_demo_user.c-78"></a>

### `rt_post`

```c
static int rt_post(int fd, uint32_t id, uint64_t value)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

以 id 与 value 构造 rtw_io_req 并下发 RTW_IOC_POST ioctl，返回其结果。

**位置**：`apps/c/rt_demo_user.c:78`

<a id="fn-apps-c-rt_demo_user.c-84"></a>

### `rt_get`

```c
static int rt_get(int fd, uint32_t id, uint64_t *value)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 RTW_IOC_GET_OUT 读取 worker 输出值，成功时写回出参 value 并返回 0。

**位置**：`apps/c/rt_demo_user.c:84`

<a id="fn-apps-c-rt_demo_user.c-94"></a>

### `create_worker`

```c
static int create_worker(int fd, const char *name, uint32_t tpl, uint8_t prio, uint32_t cpu, uint64_t period_ns, const uint64_t *params, const char *dev)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

填充 rtw_create_req（ABI 版本、模板、名字、设备、优先级、CPU、周期与参数）后下发 CREATE，成功返回新 id。

**位置**：`apps/c/rt_demo_user.c:94`

<a id="fn-apps-c-rt_demo_user.c-115"></a>

### `map_shm`

```c
static struct rtw_shm *map_shm(int fd, uint32_t id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按偏移 id<<12 映射 4096 字节共享页，magic/version 不符则解除映射并返回 NULL。

**位置**：`apps/c/rt_demo_user.c:115`

<a id="fn-apps-c-rt_demo_user.c-127"></a>

### `stop_destroy`

```c
static void stop_destroy(int fd, int id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

先下发 STOP，再轮询统计最多一千次等待进入 STOPPED/CREATED，最后 DESTROY。

**位置**：`apps/c/rt_demo_user.c:127`

<a id="fn-apps-c-rt_demo_user.c-141"></a>

### `print_proc_rtworkers`

```c
static void print_proc_rtworkers(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

打开并读取 /proc/rtworkers 内容到标准输出，两侧加分隔线；打不开则打印错误码。

**位置**：`apps/c/rt_demo_user.c:141`

<a id="fn-apps-c-rt_demo_user.c-158"></a>

### `main`

```c
int main(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

在 /dev/rtctl 上演示 event 延迟统计、周期 worker 与 watchdog 安全值，打印 /proc/rtworkers 后清理。

<details>
<summary>函数内备注（7 条）</summary>

**第 173 行**

> ── 1. event 模板：post → worker → out_seq ─────────────────

**第 174 行（行尾）**

> gain 2.0

**第 200 行（行尾）**

> 100ms 兜底

**第 235 行**

> ── 2. 周期 worker：1ms 优先，失败回退 10ms ───────────────

**第 237 行（行尾）**

> gain 1.0

**第 274 行**

> ── 3. watchdog：停掉 pulse 后输出安全值 ──────────────────

**第 304 行**

> 清理顺序：wd → pulse → evt

</details>

**位置**：`apps/c/rt_demo_user.c:158`

<a id="mod-apps-c-rt_event_test.c"></a>

## apps/c/rt_event_test.c — /dev/rtctl event 模板的用户态端到端测试

`apps/c/rt_event_test.c` · 4 个函数 · 4 个有说明 · 164 行

流程（与内核 rt_event_test 的容量测试一致）：
1. create event worker（period=0，无限等待事件，gain=2.0）；
2. start 之前投 100 条事件：前 64 条入环，后 36 条 ENOSPC；
3. start，等 processed=64，output=last(64)*2=128；
4. 再投 20 条（101..120），等 processed=84、output=240；
5. mmap 共享页核对环计数；
6. stop（worker 此时阻塞在事件等待）→ 等 STOPPED → destroy。

成功输出 [PASS] 行和 "=== Done ==="，供 tests/suites/apps.py 解析。

<a id="fn-apps-c-rt_event_test.c-34"></a>

### `check`

```c
static void check(int cond, const char *what)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按条件打印 [PASS] 或 [FAIL] 及描述，条件为假时把全局 g_fail 置 1。

**位置**：`apps/c/rt_event_test.c:34`

<a id="fn-apps-c-rt_event_test.c-44"></a>

### `rt_stats`

```c
static int rt_stats(int fd, uint32_t id, struct rtw_stats *st)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

先清零统计结构并填入 id，再用 RTW_IOC_STATS ioctl 读取并返回结果。

**位置**：`apps/c/rt_event_test.c:44`

<a id="fn-apps-c-rt_event_test.c-51"></a>

### `wait_processed`

```c
static int wait_processed(int fd, uint32_t id, uint64_t want, int timeout_ms)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

每隔 1 毫秒查一次统计，直到 events_processed 达到 want 返回 0，超时返回 -1。

**位置**：`apps/c/rt_event_test.c:51`

<a id="fn-apps-c-rt_event_test.c-63"></a>

### `main`

```c
int main(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

创建 event worker 后验证环容量 64/36 ENOSPC、启动处理、统计与共享页计数，最后 stop/destroy 并返回 g_fail。

<details>
<summary>函数内备注（3 条）</summary>

**第 79 行（行尾）**

> 无限等待事件

**第 80 行（行尾）**

> gain 2.0

**第 134 行**

> 共享页：环计数与 magic/version 对用户态可见

</details>

**位置**：`apps/c/rt_event_test.c:63`

<a id="mod-apps-c-rtctl.c"></a>

## apps/c/rtctl.c — rtctl — /dev/rtctl 用户态控制面

`apps/c/rtctl.c` · 18 个函数 · 18 个有说明 · 514 行

ABI 必须与 include/rt/rt_worker.h 逐字节一致（结构体尺寸被编进 ioctl 号，
尺寸不同就是另一个命令）。内核那份是权威定义；这里只做用户态副本。

用法：
rtctl create <name> <tpl> <prio> <cpu> <period_ms> [p0..p7] [dev:<name>]
rtctl start <id>
rtctl stop <id>
rtctl destroy <id>
rtctl set <id> <value>
rtctl post <id> <value>     # 投递一个事件（event 模板）
rtctl params <id> [p0..p7]
rtctl get <id>
rtctl stats <id>
rtctl list

模板：
periodic_io  p0=gain_q16 p1=offset p2=min p3=max
pid          p0=kp_q16 p1=ki_q16 p2=kd_q16 p3=out_min
p4=out_max p5=setpoint p6=integral_limit
watchdog     p0=target_id p1=timeout_ns p2=safe_output
event        同 periodic_io；period_ms=0 表示无限等待事件

<a id="fn-apps-c-rtctl.c-41"></a>

### `tpl_name`

```c
static const char *tpl_name(uint32_t id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把模板 id 映射成 periodic_io/pid/watchdog/event 字符串，未知时返回问号。

**位置**：`apps/c/rtctl.c:41`

<a id="fn-apps-c-rtctl.c-57"></a>

### `state_name`

```c
static const char *state_name(uint32_t s)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把 worker 状态值映射成 CREATED/RUNNING/STOPPED/FAULTED，未知一律返回 FREE。

**位置**：`apps/c/rtctl.c:57`

<a id="fn-apps-c-rtctl.c-73"></a>

### `tpl_id`

```c
static uint32_t tpl_id(const char *s)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把模板名字符串比较映射成 RTW_TPL_* 常量，未知返回 0。

**位置**：`apps/c/rtctl.c:73`

<a id="fn-apps-c-rtctl.c-86"></a>

### `open_dev`

```c
static int open_dev(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

以读写方式打开 /dev/rtctl，失败时向 stderr 打印错误并返回 -1。

**位置**：`apps/c/rtctl.c:86`

<a id="fn-apps-c-rtctl.c-96"></a>

### `parse_u32`

```c
static int parse_u32(const char *s, uint32_t *out)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 strtoul 按自动进制解析无符号整数，空串或带尾随字符返回 -1，否则写出参返回 0。

**位置**：`apps/c/rtctl.c:96`

<a id="fn-apps-c-rtctl.c-106"></a>

### `cmd_create`

```c
static int cmd_create(int argc, char **argv)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

解析名字/模板/优先级/CPU/周期毫秒及参数与设备选项，校验后下发 RTW_IOC_CREATE 并打印新 id。

**位置**：`apps/c/rtctl.c:106`

<a id="fn-apps-c-rtctl.c-167"></a>

### `cmd_id_op`

```c
static int cmd_id_op(const char *cmd, uint64_t req, int argc, char **argv)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

解析 id 后对 /dev/rtctl 下发给定的 ioctl 请求，成功后打印 id 与 ok。

**位置**：`apps/c/rtctl.c:167`

<a id="fn-apps-c-rtctl.c-192"></a>

### `cmd_set`

```c
static int cmd_set(int argc, char **argv)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

解析 id 与数值，用 RTW_IOC_SET_IN 下发给 worker，失败时打印错误。

**位置**：`apps/c/rtctl.c:192`

<a id="fn-apps-c-rtctl.c-222"></a>

### `cmd_post`

```c
static int cmd_post(int argc, char **argv)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

解析 id 与数值，用 RTW_IOC_POST 推送一个事件，成功打印 id 与值。

**位置**：`apps/c/rtctl.c:222`

<a id="fn-apps-c-rtctl.c-253"></a>

### `cmd_params`

```c
static int cmd_params(int argc, char **argv)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

解析 id 与最多 RTW_PARAM_MAX 个参数，用 RTW_IOC_SET_PARAMS 更新运行期参数。

**位置**：`apps/c/rtctl.c:253`

<a id="fn-apps-c-rtctl.c-292"></a>

### `map_worker_shm`

```c
static struct rtw_shm *map_worker_shm(int fd, uint32_t id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按偏移 id<<12 映射 4096 字节共享页，magic/version 不符则打印错误、解除映射并返回 NULL。

<details>
<summary>函数内备注（1 条）</summary>

**第 294 行**

> 页对齐令牌：id &lt;&lt; 12。musl 的 mmap wrapper 会拒绝非对齐 offset。

</details>

**位置**：`apps/c/rtctl.c:292`

<a id="fn-apps-c-rtctl.c-309"></a>

### `cmd_shm`

```c
static int cmd_shm(int argc, char **argv)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

映射 worker 共享页并打印状态、输入输出、序号、tick 与执行/唤醒延迟以及事件环计数。

**位置**：`apps/c/rtctl.c:309`

<a id="fn-apps-c-rtctl.c-345"></a>

### `cmd_shmset`

```c
static int cmd_shmset(int argc, char **argv)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

映射共享页后用原子写把 value 落到 input 字段，打印结果并解除映射。

**位置**：`apps/c/rtctl.c:345`

<a id="fn-apps-c-rtctl.c-377"></a>

### `cmd_get`

```c
static int cmd_get(int argc, char **argv)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

解析 id 后用 RTW_IOC_GET_OUT 读取输出值，打印 id、value 与 seq。

**位置**：`apps/c/rtctl.c:377`

<a id="fn-apps-c-rtctl.c-402"></a>

### `cmd_stats`

```c
static int cmd_stats(int argc, char **argv)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

解析 id 后用 RTW_IOC_STATS 读取统计，分行打印状态、周期、tick、延迟、输入输出与事件计数。

**位置**：`apps/c/rtctl.c:402`

<a id="fn-apps-c-rtctl.c-438"></a>

### `cmd_list`

```c
static int cmd_list(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 RTW_IOC_LIST 取全部 worker，按表格打印 id/状态/模板/优先级/CPU/周期/tick/名字。

**位置**：`apps/c/rtctl.c:438`

<a id="fn-apps-c-rtctl.c-462"></a>

### `usage`

```c
static void usage(const char *argv0)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

逐行打印 rtctl 各子命令与其参数的用法，并列出可用模板名。

**位置**：`apps/c/rtctl.c:462`

<a id="fn-apps-c-rtctl.c-478"></a>

### `main`

```c
int main(int argc, char **argv)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按 argv[1] 分派到 create/start/stop/destroy/set/post/params/shm 子命令，未知或 help 时打印用法。

**位置**：`apps/c/rtctl.c:478`

<a id="mod-apps-c-rtctl_abi.h"></a>

## apps/c/rtctl_abi.h — /dev/rtctl 用户态 ABI（权威定义在 include/rt/rt_worker.h）。

`apps/c/rtctl_abi.h` · 1 个函数 · 1 个有说明 · 189 行

结构体尺寸被编进 ioctl 号，改了这里必须同步内核那份。

<a id="fn-apps-c-rtctl_abi.h-184"></a>

### `rtw_shm_events`

```c
static inline struct rtw_event_ring *rtw_shm_events(struct rtw_shm *s)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把共享页指针加上 RTSHM_EVENT_OFFSET 偏移，返回其中的事件环指针。

**位置**：`apps/c/rtctl_abi.h:184`

<a id="mod-apps-c-rtps.c"></a>

## apps/c/rtps.c — rtps — 观察 /proc 里的实时任务（RT policy + priority）

`apps/c/rtps.c` · 6 个函数 · 6 个有说明 · 179 行

背景：busybox ps 只读 /proc/<pid>/stat，看不到调度策略。内核的
/proc/<pid>/status 现在提供：

Policy:	1 (SCHED_FIFO)
RtPrio:	1

本程序枚举 /proc 下的数字 PID 目录，解析 status，按 PID 排序输出
PID / PPID / POLICY / PRIO / STATE / NAME。用法：

rtps          # 所有任务
rtps -r       # 只看 RT 任务（policy != SCHED_NORMAL 或 prio > 0）

静态链接（和 racetest 一样），不依赖 rootfs 里的动态 loader。

<a id="fn-apps-c-rtps.c-38"></a>

### `usage`

```c
static void usage(const char *argv0)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

打印 rtps 用法，说明 -r 只显示实时任务、-h 显示帮助。

**位置**：`apps/c/rtps.c:38`

<a id="fn-apps-c-rtps.c-45"></a>

### `policy_name`

```c
static void policy_name(int num, char *out, size_t outsz)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把调度策略数字 1/2 映射成 SCHED_FIFO/SCHED_RR，其它值一律写 SCHED_NORMAL。

**位置**：`apps/c/rtps.c:45`

<a id="fn-apps-c-rtps.c-60"></a>

### `read_task`

```c
static int read_task(const char *pid_name, struct task_info *ti)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读 /proc/<pid>/status 并解析 Name/State/Pid/PPid/Policy/RtPrio，无策略名时用数字补出策略字符串。

<details>
<summary>函数内备注（1 条）</summary>

**第 73 行**

> PID 目录名只可能是数字；限宽避免 -Wformat-truncation 噪声

</details>

**位置**：`apps/c/rtps.c:60`

<a id="fn-apps-c-rtps.c-108"></a>

### `cmp_pid`

```c
static int cmp_pid(const void *a, const void *b)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按 pid 大小比较两个 task_info，用作 qsort 的排序函数。

**位置**：`apps/c/rtps.c:108`

<a id="fn-apps-c-rtps.c-115"></a>

### `is_rt`

```c
static int is_rt(const struct task_info *ti)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

判断任务是否实时：优先级大于 0，或策略字符串不是 SCHED_NORMAL。

**位置**：`apps/c/rtps.c:115`

<a id="fn-apps-c-rtps.c-120"></a>

### `main`

```c
int main(int argc, char **argv)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

解析 -r/-h 后遍历 /proc 数字目录读取任务信息，可按 RT 过滤，按 pid 排序后打印表格并标注 [RT]。

**位置**：`apps/c/rtps.c:120`

<a id="mod-apps-c-task_completion_test.c"></a>

## apps/c/task_completion_test.c — completion 模块的用户态 A/B 验收

`apps/c/task_completion_test.c` · 6 个函数 · 6 个有说明 · 205 行

需要 TASK_TEST=1 的内核(暴露三个测试 syscall, 见
kernel/syscall/syscall.c 的 TASK_INFRA_TEST 段)。

task-completion broadcast <n> <attempts>

每轮 fork n 个子进程, 每个阻塞在同一个 completion 上；父进程等它们
全部 armed 后, 记录 t0, 调用 COMPLETE 唤醒全部, 子进程各自记录被唤醒
的时刻。输出：
- ready_cpu0/1：等待者分布在哪些核
- total_us / max_us：COMPLETE 到最后一个等待者醒来的墙钟/最大延迟

判据：
- 新内核 completion 唤醒走 sched_unblock() + resched IPI, 跨核等待者
应在几十~几百 us 内全醒；
- 基线内核(同样的测试 syscall, 但内部是裸 state=READY+sched_enqueue)
跨核等待者要等目标核下一次 tick(100Hz = 10ms)。

用户态程序里手抄的伪 syscall 号必须与 kernel/syscall/syscall_internal.h
的 AVATAR_SYS_COMPLETION_* 一致。

<a id="fn-apps-c-task_completion_test.c-49"></a>

### `cpu_now`

```c
static int cpu_now(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

调用 syscall getcpu（x86_64 用 309、其它用 168）取当前 CPU 号，失败返回 -1。

<details>
<summary>函数内备注（1 条）</summary>

**第 53 行**

> getcpu：x86_64 = 309, aarch64/riscv = 168(通用号)

</details>

**位置**：`apps/c/task_completion_test.c:49`

<a id="fn-apps-c-task_completion_test.c-64"></a>

### `now_ns`

```c
static uint64_t now_ns(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 clock_gettime(CLOCK_MONOTONIC) 取时间并换算成纳秒，失败返回 0。

**位置**：`apps/c/task_completion_test.c:64`

<a id="fn-apps-c-task_completion_test.c-73"></a>

### `parse_int`

```c
static int parse_int(const char *s, int fallback)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 strtol 解析十进制，空串、带尾随字符或不在 1..1000000 时返回默认值。

**位置**：`apps/c/task_completion_test.c:73`

<a id="fn-apps-c-task_completion_test.c-86"></a>

### `yield_until`

```c
static void yield_until(uint64_t ns)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

循环调用 sched_yield，直到单调时钟走过 ns 纳秒。

**位置**：`apps/c/task_completion_test.c:86`

<a id="fn-apps-c-task_completion_test.c-101"></a>

### `broadcast_once`

```c
static int broadcast_once(int n, int attempt)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

mmap 共享页并 reset completion，fork n 个子进程各自 WAIT，父进程等就绪后 COMPLETE，统计唤醒延迟与 CPU 分布。

<details>
<summary>函数内备注（4 条）</summary>

**第 119 行**

> 上一轮的完成者已经全部退出；复位失败说明 completion 有残留等待者

**第 144 行**

> 等全部进入等待态；yield 让与父进程同核的等待者也能跑到 wait

**第 147 行（行尾）**

> 5ms：确保都已经睡在 completion 上

**第 162 行**

> 唤醒时所在 CPU 不一定等于就绪时；这里不再取一次 getcpu(子进程
> 已经退出), 只用时间戳判延迟。

</details>

**位置**：`apps/c/task_completion_test.c:101`

<a id="fn-apps-c-task_completion_test.c-181"></a>

### `main`

```c
int main(int argc, char **argv)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验模式为 broadcast 后取等待者数与轮数（默认 8 与 5），循环调用 broadcast_once 并按位或汇总返回码。

**位置**：`apps/c/task_completion_test.c:181`

<a id="mod-apps-c-task_ipc_test.c"></a>

## apps/c/task_ipc_test.c — task 基础设施的用户态验收(A/B 对比用)

`apps/c/task_ipc_test.c` · 9 个函数 · 9 个有说明 · 273 行

目的：证明「有新的跨核唤醒/抢占基础设施」和「没有」是可观测地不同的。
只用标准 syscall(fork/mmap/getcpu/futex/clock_gettime), 因此同一份
二进制可以同时跑在新内核和旧内核上做对照。

用法：
task-ipc dist <n>
创建 n 个子进程, 每个报告自己落在哪颗 CPU。验证 SMP 下任务确实
按 round-robin 摊开(两核时两边都应该 >0)。

task-ipc ping <iters> <attempts>
父/子用 MAP_SHARED 内存 + futex 乒乓。若两者在不同核上, 且
内核没有「远端唤醒立即 kick」/ IPI, 被唤醒方要等自己那颗核的
下一次 timer tick(本内核 100Hz = 10ms)。新内核用 resched IPI
立即唤醒。每次 attempt 打印两端 CPU 和平均往返延迟(us)。

为什么不用 pipe：pipe 的 blocked_reader/blocked_writer 是无锁检查,
存在已知的 lost-wakeup(见 apps/c/racetest.c 的 pipe 子命令)。用它
测 task 基础设施, 失败时无法区分是 IPI 坏了还是 pipe 丢了唤醒。
futex 的 wait/wake 有全局锁, 且新内核里唤醒统一走 sched_unblock()。

输出统一带 "TASKIPC " 前缀, 方便脚本 grep 和 A/B 对账。

<a id="fn-apps-c-task_ipc_test.c-60"></a>

### `cpu_now`

```c
static int cpu_now(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

调用 syscall(SYS_getcpu) 取当前 CPU 号，失败返回 -1。

**位置**：`apps/c/task_ipc_test.c:60`

<a id="fn-apps-c-task_ipc_test.c-69"></a>

### `now_ns`

```c
static uint64_t now_ns(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 clock_gettime(CLOCK_MONOTONIC) 取时间并换算成纳秒，失败返回 0。

**位置**：`apps/c/task_ipc_test.c:69`

<a id="fn-apps-c-task_ipc_test.c-78"></a>

### `busy_wait_ns`

```c
static void busy_wait_ns(uint64_t ns)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

以单调时钟忙等，直到经过 ns 纳秒。

**位置**：`apps/c/task_ipc_test.c:78`

<a id="fn-apps-c-task_ipc_test.c-86"></a>

### `futex_wait`

```c
static int futex_wait(volatile int *uaddr, int val)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

以 FUTEX_WAIT 操作和期望值 val 对给定地址调用 SYS_futex。

**位置**：`apps/c/task_ipc_test.c:86`

<a id="fn-apps-c-task_ipc_test.c-91"></a>

### `futex_wake`

```c
static int futex_wake(volatile int *uaddr, int n)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

以 FUTEX_WAKE 操作唤醒给定地址上最多 n 个等待者。

**位置**：`apps/c/task_ipc_test.c:91`

<a id="fn-apps-c-task_ipc_test.c-96"></a>

### `parse_int`

```c
static int parse_int(const char *s, int fallback)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 strtol 解析十进制，空串、带尾随字符或不在 1..1000000 时返回默认值。

**位置**：`apps/c/task_ipc_test.c:96`

<a id="fn-apps-c-task_ipc_test.c-116"></a>

### `mode_dist`

```c
static int mode_dist(int n)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

fork n 个子进程各把自己所在 CPU 记入共享页，父进程等计数到位后统计并打印各 CPU 分布。

**位置**：`apps/c/task_ipc_test.c:116`

<a id="fn-apps-c-task_ipc_test.c-179"></a>

### `ping_once`

```c
static int ping_once(int iters, int attempt)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

fork 子进程建立 futex 乒乓，父进程按轮次唤醒并测往返延迟，最后打印父子 CPU 与平均微秒。

<details>
<summary>函数内备注（1 条）</summary>

**第 226 行**

> 让子进程真正进入 futex_wait 再开始。否则第一批 ping 可能在它睡下
> 之前就到达, 测到的是「根本没睡」。5ms 足够覆盖双核调度。

</details>

**位置**：`apps/c/task_ipc_test.c:179`

<a id="fn-apps-c-task_ipc_test.c-252"></a>

### `main`

```c
int main(int argc, char **argv)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按 argv[1] 走 dist（分发统计）或 ping（往返延迟）模式，缺省为 dist，未知模式打印用法返回 2。

**位置**：`apps/c/task_ipc_test.c:252`

<a id="mod-apps-c-vmm_run.c"></a>

## apps/c/vmm_run.c — 在宿主 shell 里启动 / 接入 / 管理 Linux guest

`apps/c/vmm_run.c` · 17 个函数 · 17 个有说明 · 919 行

移植自 x-kernel: uapps/kvmm-run/kvmm-run.c

用法(宿主 busybox shell 里)：
/bin/vmm-run              启动一个 guest；已有在跑的就接入它
/bin/vmm-run -c <N>       新建一个 N vCPU 的 guest(隐含 -n)
/bin/vmm-run -a <vmid>    接入指定的那个 VM(不新建)
/bin/vmm-run -n           强制新建一个 VM, 即使已经有在跑
/bin/vmm-run -l           列出所有 VM, 不接管控制台
/bin/vmm-run -k [vmid]    停止 VM, 不接管控制台；不带 vmid 停全部

运行中(前缀键 Ctrl+T, 再按一个键)：
Ctrl+T ?        帮助
Ctrl+T d        detach：guest 留在后台, 退回宿主 shell(再跑 vmm-run 接回)
Ctrl+T k        停掉当前前台 guest 并退出
Ctrl+T l        列出所有 VM(编号供 Ctrl+T <n> 用)
Ctrl+T n        新建一个 VM 并切过去
Ctrl+T 1..8     切到 -l 列出的第 n 个 VM
Ctrl+T Ctrl+T   把 Ctrl+T 本身发给 guest

做三件事：
1. 打开 /dev/vmm, ioctl(VMM_IOC_BOOT) 让内核启动 guest —— 如果已经有
guest 在跑(上次 Ctrl+T d 留下的), 这个 ioctl 只是接入, 不重新加载
镜像；
2. 把 stdin 设成 raw mode, 然后在本进程 stdin/stdout 与 /dev/vmm
之间双向搬字节 —— 不做任何加工, ANSI 转义序列原样透传；
3. 读到前缀键就进「命令态」, 按上表处理。

注意： 同一时刻只支持一个交互式 helper：前台 VM 是内核里的一个全局变量
(g_vmm_fg_vmid), B 一 attach, A 那个终端的读写就变成了 B 的 VM ——
A 的窗口会不知不觉变成另一个 guest 的窗口。`-a` 的语义是「切走前台」,
不是「再开一个会话」。

── 为什么是 Ctrl+T ────────────────────────────────────────────

从前用的是 Ctrl+](停)和 Ctrl+[(分离), 两个都不好：

- Ctrl+] 是 telnet 的转义键；
- Ctrl+[ 就是 ESC(0x1b), 而方向键/Home/End/Fn 发出的正是以 0x1b 开头的
转义序列(上箭头 = \x1b[A)。宿主内核 raw mode 下 read() 每次只返回
1 字节, 所以 \x1b[A 会拆成三次到达 —— 见到 0x1b 就分离的话, guest 里的
方向键会全部失效。当时靠一个 ~40ms 的 poll 窗口消歧(有后续字节就当
转义序列), 能用但很 hack。

换成「前缀 + 单键命令」(screen / tmux / QEMU 都是这个路子)之后不再需要
那个消歧窗口：前缀之后的那个字节一定是命令, 不可能是转义序列的一部分。
顺带白送一个好处 —— ESC 不再被拦截, guest 里的方向键天然可用。

前缀选 Ctrl+T(0x14), 因为它是唯一一个既好按, 又没被占用的控制字节：

Ctrl+A   QEMU -nographic 的转义前缀(三个架构的 Makefile 都这么启)
+ screen / minicom / picocom —— 根本到不了这个内核
Ctrl+B   tmux
Ctrl+]   telnet
Ctrl+C   必须留给 guest(SIGINT)
Ctrl+^   Ctrl+_   没人用, 但要 Shift+6 / Shift+-, 部分键盘布局和终端
压根发不出这两个字节
Ctrl+\   kermit, 而且是 guest 里的 SIGQUIT

代价：guest 里 bash 的 transpose-chars(罕用)让位给控制台；要发字面量
就按两次(Ctrl+T Ctrl+T), 或者单按一次前缀, 等它超时自动补发。

为什么要在用户态做这件事(而不是内核直接接管控制台)：宿主 tty 层
(signal_check_uart / uart_ringbuf)始终独占真实 UART, 内核里不存在
第二个消费者, 也就不需要「控制台归属」仲裁。guest 的字节全部经本进程
中转, 内核侧只有控制台 vdev 的两个环形缓冲。

<a id="fn-apps-c-vmm_run.c-170"></a>

### `delay_ms`

```c
static void delay_ms(int ms)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 poll(NULL, 0, ms) 睡眠指定毫秒。

<details>
<summary>函数内备注（1 条）</summary>

**第 172 行**

> 用 poll(NULL, 0, ms) 当睡眠：内核的 ppoll 路径已经实测可用,
> 而我们不需要纳秒精度。避免再依赖 nanosleep/sched_yield 是否实现。

</details>

**位置**：`apps/c/vmm_run.c:170`

<a id="fn-apps-c-vmm_run.c-179"></a>

### `write_all`

```c
static int write_all(int fd, const char *buf, size_t len)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

循环 write 直到写满 len 字节，EINTR 时重试，其它错误返回 -1。

**位置**：`apps/c/vmm_run.c:179`

<a id="fn-apps-c-vmm_run.c-196"></a>

### `restore_tty`

```c
static void restore_tty(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

若保存过终端属性，则用 TCSANOW 把标准输入恢复成之前保存的设置。

**位置**：`apps/c/vmm_run.c:196`

<a id="fn-apps-c-vmm_run.c-210"></a>

### `on_fatal_signal`

```c
static void on_fatal_signal(int sig)
```

被信号打死也要把 tty 还原。

不还原的话宿主 shell 会留在无回显, 无行编辑的状态 —— 而这台机器
只有一个控制台, 你几乎没有别的办法把它改回来(Ctrl+C 在 raw mode 下
是普通字节, 连信号都不是)。helper 收到的常见信号是 SIGHUP/TERM。
SIGKILL 拦不住, 那种情况下只能重开 QEMU。

**位置**：`apps/c/vmm_run.c:210`

<a id="fn-apps-c-vmm_run.c-216"></a>

### `enter_raw_mode`

```c
static int enter_raw_mode(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

保存终端属性并注册 SIGHUP/INT/TERM/QUIT 处理器，切到 raw 模式但补回 ICRNL 后写回终端。

<details>
<summary>函数内备注（1 条）</summary>

**第 231 行**

> cfmakeraw 清掉了 ICRNL, 那会让回车以 &#x27;\r&#x27; 原样送到 guest。
> Linux 的 tty 层自己会做 ICRNL, 所以这里加不加都能用；加上的好处是
> 宿主侧先统一成 &#x27;\n&#x27;, helper 转发时不必关心用户敲的是 CR 还是 LF。

> 顺带一提：cfmakeraw 也清掉了 ISIG, 所以 raw mode 下 Ctrl+C 不再变成
> 信号, 而是当普通字节透传 —— 这正是「退出键不能复用 Ctrl+C」的原因
> (guest 需要它), 也是上面那个信号处理器存在的理由(本地 Ctrl+C
> 救不了你)。内核那边的判据在 tty.c：只有 c_lflag 的 ISIG 位置着时,
> 0x03 才会被转成 SIGINT 而不是塞进环形缓冲。

</details>

**位置**：`apps/c/vmm_run.c:216`

<a id="fn-apps-c-vmm_run.c-257"></a>

### `ioctl_failed`

```c
static void ioctl_failed(const char *what)
```

ioctl 失败时的报错。

注意： ENOTTY 是个很有误导性的症状, 值得单独说：内核把不认识的 ioctl 统一
回成 ENOTTY —— 设备层虽然回的是 -ENOSYS, 但 syscall 层会把 ENOSYS
当成"这不是本设备管的"继续往下走, 最后落到 -ENOTTY
(kernel/syscall/fs/ioctl.c:65)。而 _IOC 把 sizeof 编进了操作码,
所以结构体尺寸对不上和内核太旧没有这个 ioctl 是同一个症状。
看到它就往这两个方向查, 别去怀疑设备开错没有。

**位置**：`apps/c/vmm_run.c:257`

<a id="fn-apps-c-vmm_run.c-274"></a>

### `state_name`

```c
static const char *state_name(uint32_t s)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把 VM 状态值 1/2/4 映射成 LOADING/RUNNING/DYING，其它返回问号。

<details>
<summary>函数内备注（1 条）</summary>

**第 276 行**

> 与 include/vmm/vmm.h 的 VM_* 对应(3 是空号, 别问, 问就是历史)

</details>

**位置**：`apps/c/vmm_run.c:274`

<a id="fn-apps-c-vmm_run.c-290"></a>

### `fetch_list`

```c
static int fetch_list(int fd, struct vmm_list_req *req)
```

取一次列表。返回 0 成功, -1 失败(errno 已置)。

**位置**：`apps/c/vmm_run.c:290`

<a id="fn-apps-c-vmm_run.c-306"></a>

### `print_vm_list`

```c
static void print_vm_list(int fd)
```

打印 VM 表。第 1 列的序号就是 `Ctrl+T <n>` 用的那个 n ——
刻意不用 vmid 当序号：vmid 从 1 开始自增, 到 255 回绕(vm.c 的
g_next_vmid), 长会话里 vm9 后面接着的是 vm10 而不是「第 10 个」,
两个编号混用会很难解释。序号就是列表里的位置, 自己看自己数。

注意： 序号映射会随 VM 生死变化(列表按 vmid 升序), 所以 Ctrl+T <n> 每次
都现场重取列表, 不缓存。

**位置**：`apps/c/vmm_run.c:306`

<a id="fn-apps-c-vmm_run.c-330"></a>

### `print_help`

```c
static void print_help(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

打印 Ctrl+T 前缀控制台的按键帮助：detach、stop、list、新建、切换、帮助与字面 Ctrl+T。

**位置**：`apps/c/vmm_run.c:330`

<a id="fn-apps-c-vmm_run.c-352"></a>

### `console_new_vm`

```c
static void console_new_vm(int fd)
```

Ctrl+T n —— 新建一个 VM 并切过去。

不复用 main() 里那个 2000×1ms 的重试循环：那是为「上一个 guest 还没收完尾」
设计的, 而这里更常见的失败是池满(池满), 内核立刻回 -EBUSY,
再重试 2000 次只是白卡两秒 —— 更糟的是每次重试内核都要打两行日志, 而
klog 是持全局锁, 关中断逐字节写的, 115200 下就是十几秒的关中断自旋。
所以 EBUSY 直接认输。

**位置**：`apps/c/vmm_run.c:352`

<a id="fn-apps-c-vmm_run.c-374"></a>

### `console_switch_vm`

```c
static void console_switch_vm(int fd, unsigned idx)
```

Ctrl+T <n> —— 切到列表里的第 n 个 VM。不重新 boot, 只是改前台。

**位置**：`apps/c/vmm_run.c:374`

<a id="fn-apps-c-vmm_run.c-400"></a>

### `run_command`

```c
static int run_command(unsigned char c, int fd, int *quit, int *detach)
```

处理前缀之后的那个字节。返回 1 表示「要收尾了, 本块剩余字节丢弃」。

<details>
<summary>函数内备注（1 条）</summary>

**第 428 行**

> 未绑定的键：只提示, 不假装成功。注意这里不会把该字节转发给 guest
> —— 前缀已经消费掉它了, 想发字面量得用 Ctrl+T Ctrl+T。

</details>

**位置**：`apps/c/vmm_run.c:400`

<a id="fn-apps-c-vmm_run.c-438"></a>

### `parse_u32`

```c
static int parse_u32(const char *s, uint32_t *out)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 strtoul 解析十进制无符号整型并检查 errno 与越界，成功写出参返回 0，否则返回 -1。

**位置**：`apps/c/vmm_run.c:438`

<a id="fn-apps-c-vmm_run.c-465"></a>

### `cmd_kill`

```c
static int cmd_kill(int fd, int have_vmid, uint32_t vmid)
```

-k：停止 VM, 不接管控制台。

这里不需要再发 DETACH 兜底 —— 内核侧给 close 加了「会话 owner」守卫
(见 kernel/fs/pseudofs/vmm_dev.c 的 g_vmm_owner_pid)：只有做过
BOOT/BOOT_EX/ATTACH 的那个任务才有资格在 close 里停 VM, 而 `-k` 从没
BOOT 过, 它的 close 天然什么都不做。

从前那种"自己发 DETACH 保护自己"的写法反而是有害的：g_vmm_detached 是
会话级全局标志, `-k` 置了它, 就可能被真正 owner 的 close 消费掉 ——
用户明确要停的 guest 会被当成"分离"留在后台继续烧 CPU。

<details>
<summary>函数内备注（2 条）</summary>

**第 478 行**

> 不带 vmid：停全部 —— 多 VM 下「后台那个 guest」已经没有明确定义了

**第 488 行**

> 停止是异步的：vm_request_stop 只置标志, vCPU 任务跑到下一个安全点
> 才收尾。所以这里报的是&quot;已请求&quot;, 不是&quot;已停&quot;。紧接着 `-k; vmm-run`
> 大概率会撞上&quot;上一个还没收完尾&quot;, 那时按提示等一下即可。

</details>

**位置**：`apps/c/vmm_run.c:465`

<a id="fn-apps-c-vmm_run.c-503"></a>

### `usage`

```c
static void usage(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

向 stderr 打印 vmm-run 的 -c/-a/-n/-l/-k 选项说明与串口快捷键提示。

**位置**：`apps/c/vmm_run.c:503`

<a id="fn-apps-c-vmm_run.c-517"></a>

### `main`

```c
int main(int argc, char **argv)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

解析 -k/-c/-n/-l/-a 后打开 /dev/vmm，分别执行列表、停止、ATTACH 接入或带重试的 BOOT/BOOT_CFG 启动流程。

<details>
<summary>函数内备注（37 条）</summary>

**第 532 行**

> -k 的 vmid 是可选的：下一个参数是纯数字就当它的目标

**第 545 行（行尾）**

> 已有 VM 在跑时也再起一个, 而不是接入

**第 565 行**

> -c 的语义是“新建一个 N vCPU 的 VM”(用户明确选择的行为)。它不能和
> 只读/接入/停止类选项混用：
> -a 只改前台, -l 只查询, -k 只停止, 都没有“新建”的含义。
> -n 与 -c 同义(-c 隐含 -n), 允许同时给。

**第 585 行**

> -l / -k 都不接管控制台：查完/停完就走。

> 它们从没 BOOT 过, 所以内核侧那个 owner 守卫会让它们的 close 变成
> 空操作 —— 这正是「`vmm-run -l` 不该把别人正在用的 guest 杀掉」的
> 保证所在(从前没有这道守卫时, 一个纯查询动作真的会把 guest 停掉)。

**第 603 行**

> ── 接入指定 VM, 或启动/接入 ──────────────────────────

**第 605 行（行尾）**

> 本次实际选中/创建的 VM(0 = 不知道)

**第 606 行（行尾）**

> 实际核数；0 = 构建默认, 不显示

**第 609 行**

> -a 只改前台, 不碰 guest 的生命周期。它绕过 vmm_dev_boot, 所以也
> 不会去重置内核里的 g_vmm_detached —— 那个标志由 owner 的 close
> 负责复位, 进到这里必然是 0。

**第 629 行（行尾）**

> 查不到就当没有, 后面 BOOT 会给出结论

**第 637 行**

> 带 -c 时走 VMM_IOC_BOOT_CFG(新内核才有)。不带 -c 时仍用
> BOOT/BOOT_EX —— 旧内核也能继续用, 行为与从前完全一致。

**第 662 行**

> 失败通常是「上一个 guest 还没收完尾」—— 它的 vCPU 任务要跑到
> 下一个安全点才退出, 期间 guest_loader 的重入保护会拒绝。内核
> 不做定时等待, 隔 1ms 重试即可。

> 但池满(池满)是立刻失败, 重试无意义的, 早点认输 ——
> 而且每次重试内核都打两行日志, klog 是关中断写的, 重试 2000 次
> 就是十几秒的关中断自旋(顺带把别的核堵在 klog 锁上)。

**第 677 行**

> EINVAL 是配置错误(核数不支持 / rootfs 缺对应 DTB / 与运行中
> VM 的核数不符), 重试没有意义；而且内核每次都会打日志,
> 2000 次重试 = 十几秒的关中断自旋。直接认输。

**第 713 行**

> 不致命：没有 raw mode 就退化成行缓冲, 照样能用

**第 716 行**

> 报一下接的是哪个 vmid —— 光看 &quot;guest started&quot; 分不出是 vm1 还是 vm2,
> 而多 VM 下这正是最容易搞混的地方(banner 里那个就是内核当前认定的
> 前台, 也正是 write()/read() 会落到的那个 VM)。

**第 723 行**

> force_new 优先：`-n` 在有 guest 在跑时 attaching 也是 1, 但语义是
> &quot;又新建了一个&quot;, 报成 attached 会让人以为接到了老的那个。

**第 728 行**

> 优先用 ioctl 的出参：多 VM 下 g_vmm_fg_vmid 可能在 boot 返回后
> 被另一个 helper 改掉, 回读列表反而可能报错号码。出参不在
> (-a 以外的旧路径)时才退回查列表。

**第 744 行**

> ── 双向搬运 ──────────────────────────────────────────

**第 751 行（行尾）**

> Ctrl+T k：停止并退出

**第 752 行（行尾）**

> Ctrl+T d：分离

**第 753 行（行尾）**

> guest 自己没了(或被别的进程停掉了)

**第 754 行（行尾）**

> 已看到前缀, 正在等命令字节

**第 755 行（行尾）**

> 命令窗口已过去的近似毫秒数

**第 761 行**

> 等命令字节时用有限超时, 其余时候一直等。

> 这个超时是必须的：没有它的话, 单独误按一次 Ctrl+T 会把
> 下一次按键当成命令执行 —— 而其中 &#x27;k&#x27; 是&quot;停掉前台 VM 并退出&quot;。
> screen/tmux 的做法是超时后把前缀原样发给 guest, 这里照做。

**第 779 行**

> 孤立的前缀：当成用户想发一个字面 Ctrl+T, 补发出去

**第 788 行**

> guest → 宿主终端。

> 注意： HUP/ERR 必须当成会话结束来处理。只把 fd 里的数据抽干就
> 继续下一轮 monitor 的话：guest 没了的时候 read() 返回的是
> -EAGAIN(前台已经查不到), 内层循环 break 而 quit 仍是 0,
> 外层 poll 又会立刻报 HUP —— 于是原地打起 100% CPU 的死循环,
> 而且 raw mode 下 ISIG 是关的, Ctrl+C 连信号都不产生,
> 单控制台的机器上你退不出来。这个坑在&quot;guest 自己 poweroff&quot;
> 时本来就会踩到, 而 `-k` 让它变成日常路径。

**第 813 行（行尾）**

> 取空了 + 报了 HUP ⇒ guest 真的没了

**第 818 行**

> 出错, 或者 guest 已经退出

**第 826 行**

> 宿主键盘 → guest(前缀之外的字节原样透传)

**第 836 行（行尾）**

> stdin 关了

**第 846 行（行尾）**

> 前缀本身不转发

**第 852 行**

> 命令态：这一个字节一定是命令。

> raw mode 下 read() 每次只回 1 字节(内核 file_io.c 的 raw
> 分支收够一个就 break), 所以 n 通常是 1, cmd_mode 不会跨
> read 悬着。但这里仍按「一个 buf 里可能有多个字节」写,
> 不去依赖那个实现细节 —— 万一 tcsetattr 失败退化成行缓冲,
> 一整行会一次到达, 这时只有前缀后的第一个字节是命令。

**第 863 行（行尾）**

> Ctrl+T Ctrl+T → 字面量

**第 868 行（行尾）**

> 收尾命令：本块剩余字节丢弃(和从前一致)

**第 880 行**

> ── 收尾 ──────────────────────────────────────────────

**第 882 行**

> 先告诉内核「这次 close 不要停 guest」, 再退出。
> close 之后 vCPU 任务照常跑, 输出继续进 TX 环形缓冲；下次
> vmm-run 接入时会把缓冲里攒下的内容一并吐出来(类似 screen -r)。

**第 896 行**

> guest 自己结束了(poweroff), 或者被另一个进程 `vmm-run -k` 停掉了。
> 不用再发 STOP —— 那个 VM 已经不在池里了。

**第 909 行**

> close 是兜底：即使上面没来得及 ioctl(STOP), 内核侧默认也会停 guest
> (分离标志只在 DETACH 后为真)。这里不等 guest 真的停下来：vCPU
> 任务会在下一个安全点自己收尾, 而我们要的是立刻把控制台还给宿主 shell。

</details>

**位置**：`apps/c/vmm_run.c:517`
