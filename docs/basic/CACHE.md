# 缓存操作（cache.h）

> **一句话**：C 代码要维护缓存，只用 `include/cache.h` 的动词；
> 缓存指令只允许出现在 `include/<arch>/cache_impl.h`（以及 riscv 的
> `include/riscv64/rvplatform.h`）里，`.S` 除外。

## 接口

```c
/* 按区间（最常用）*/
clean_dcache_range(addr, size);                  /* 写回，保留在缓存里 */
invalidate_dcache_range(addr, size);             /* 失效，不写回（脏数据会丢） */
clean_and_invalidate_dcache_range(addr, size);   /* 写回并失效 */

sync_caches();          /* 等待之前的 cache 操作完成（CMO 之后的完成屏障） */
sync_icache_all();      /* 让刚写入的指令可见（加载器打补丁 / 自改码 / JIT）*/
get_cache_line_size();  /* 缓存行大小（range 操作的步长）*/
init_cache();           /* 探测/设定行大小；由 kernel_main 在 platform_init 之后调用 */
```

区间动词的首尾**非整行**部分会自动做 clean+invalidate（`invalidate_dcache_range`
否则会把邻居的脏数据一起丢掉）。它中间的部分是"失效不写回"，这是本意不是缺陷 ——
用之前先确认区间里没有未写回的数据。

## 各架构实现

| | clean | invalidate | clean+invalidate | icache |
|---|---|---|---|---|
| **aarch64** | `dc cvac` | `dc ivac` | `dc civac` | `ic iallu` + `dsb ish` + `isb` |
| **riscv64 / Zicbom** | `cbo.clean` | `cbo.inval` | `cbo.flush` | `fence.i` |
| **riscv64 / T-Head C906** | `dcache.cva` | `dcache.iva` | `dcache.civa` | `fence.i` + `icache.iall` + `sync.i` |
| **riscv64 / 无 CMO** | 空操作 | 空操作 | 空操作 | `fence.i` |
| **x86_64** | `clflush`（或 `clwb`） | `clflushopt`(+`sfence`) 或 `clflush` | `clflush` | 序列化屏障（I/D 硬件一致） |

- **QEMU riscv64 走"无 CMO"分支**：DMA 由模拟器保证一致，三个动词都是空操作。
  在那里测不出缓存一致性问题，别拿它验证 SG2002 的 DMA 路径。
- **x86_64 的 `__CLWB__` / `__CLFLUSHOPT__` 目前没有定义**，所以三个动词实际都是
  `clflush`（clean 与 clean+invalidate 不可区分）；两个分支留着，打开宏即可启用。
- aarch64 的整 cache 操作（`dc isw/cisw` set/way）**没有实现**：ARM ARM 要求做这件事时
  其他核对缓存的访问必须停止，在本内核的 SMP 前提下不能安全使用。需要时用区间动词。

## RISC-V 为什么要多一层 `rvplatform.h`

aarch64 的 `dc`/`ic` 和 x86_64 的 `clflush`/`wbinvd` 都是**架构标准指令**，行大小也能从
`CTR_EL0` / `CPUID` 读出来 —— 所以那两套 `cache_impl.h` 自己就够了。

RISC-V 不一样：**基座 ISA 里没有 CMO**，标准扩展 Zicbom 是可选的，而 Zicbom 之前的
厂商核（T-Head C906/C910）用 CUSTOM-0 **私有编码**。于是把"这台机器到底有什么 CMO"
收敛到 `include/riscv64/rvplatform.h`（`RV_CMO_NONE` / `RV_CMO_ZICBOM` / `RV_CMO_THEAD`），
`cache_impl.h` 只做"通用动词 → 平台原语"的映射。

平台选择用 `PLATFORM_SG2002` / `__riscv_zicbom`，与 Makefile 的 `-DPLATFORM_$(name)` 一致；
新增平台只需在 `rvplatform.h` 里加一个分支。

### ⚠️ C906 整 cache 编码待确认

`rvplatform.h` 的 `rv_cmo_invalidate_all()` 用 `0x0030000b`。公开的 T-Head 编码表是
`dcache.call = 0x0010000b` / `dcache.ciall = 0x0020000b` / **`dcache.iall = 0x0030000b`**，
即 `0x0030000b` 是**失效、不写回**。

而**原来的代码把它命名为 `__c906_dcache_ciall`（清理+失效）并当作"清理"用**，还给
`clean_dcache_range()` 也走了它 —— 若该表正确，那条路径会丢弃脏数据。

现状：SG2002 的区间操作已改为按 VA 的 `cva`/`iva`/`civa`，这个整 cache 原语**目前没有
任何调用者**，疑似问题暂不可达。函数名按编码表取（invalidate）；在查手册或上板确认之前，
**不要**把它接到 clean 语义上。

### `ARCH_HAS_CUSTOM_DCACHE_RANGE`

架构/平台自己实现三个区间动词时定义它。现在**没有架构用它**（SG2002 曾用它把三个动词
都换成"整 cache 刷"，等于丢掉区间语义，已删除）。保留这个开关是为了"按 VA 的 CMO 在这块
硬件上不可靠"这种情况 —— 真要用请先上板验证，并在实现处写清为什么。

## DMA 一致性（用这些动词的场景）

| 设备 | DMA | 现状 |
|---|---|---|
| `driver/eth/virtio_net.c` | 是（描述符环 + 缓冲按物理地址交给设备） | **没有任何 cache 维护** —— QEMU 上没事（riscv64 是空操作、x86/aarch64 一致），**在 SG2002 这类非一致平台上就是 bug** |
| `driver/tpu/cvi_tpu.c`（SG2002 TDMA） | 是 | 由 userspace 经 `/dev/cvi-tpu0` 的 ioctl 触发，落到 `kernel/fs/pseudofs/dev.c` → 本 API |
| `driver/blk/sdblk.c` | 否（纯 PIO） | 不需要 |
| `driver/blk/ramblk.c` | 否（memcpy 到 RAM 窗口） | 不需要 |

注意 `driver/ion/ion.c` 里"清零缓冲区（DMA coherent 语义）"这句注释：**在 SG2002 上这个
假设不成立**，靠的是 userspace 侧的 flush/invalidate。

## 调用点

只有两个文件用这个 API：
- `kernel/fs/pseudofs/dev.c` —— TPU/ION 的 DMA 缓冲（ioctl 驱动的 flush/invalidate）；
- `kernel/vmm/guest_loader.c` —— host 写好 guest 内存后 `clean_dcache_range`。

`sync_icache_all()` 目前没有 C 调用者；`exec`/`fork` 返回路径的 I-cache 同步在 `.S` 里
（汇编用不了 C 接口），语义与这里一致，见 `kernel/task/riscv64/switch.S` 与
`boot/riscv64/exception.S`。
