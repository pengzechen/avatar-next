# tools — C API

[← 返回总览](index.md) · 4 个函数 · 说明覆盖率 100.0%

<a id="mod-tools-fdt_host_test-fdt_check.c"></a>

## tools/fdt_host_test/fdt_check.c

`tools/fdt_host_test/fdt_check.c` · 1 个函数 · 1 个有说明 · 20 行

<a id="fn-tools-fdt_host_test-fdt_check.c-10"></a>

### `main`

```c
int main(int argc, char **argv)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把 argv[1] 指向的 DTB 读入缓冲，打印 npu-pool 节点是否有 no-map、npu 节点是否有 dma-coherent，返回 0。

**位置**：`tools/fdt_host_test/fdt_check.c:10`

<a id="mod-tools-fdt_host_test-fdt_host.c"></a>

## tools/fdt_host_test/fdt_host.c — 把宿主侧的四个补丁按 guest_loader.c 的调用顺序跑一遍

`tools/fdt_host_test/fdt_host.c` · 1 个函数 · 1 个有说明 · 17 行

<a id="fn-tools-fdt_host_test-fdt_host.c-9"></a>

### `host_patch_all`

```c
int host_patch_all(unsigned char *buf, unsigned len)
```

把宿主侧的四个补丁按 guest_loader.c 的调用顺序跑一遍

**位置**：`tools/fdt_host_test/fdt_host.c:9`

<a id="mod-tools-fdt_host_test-fdt_main.c"></a>

## tools/fdt_host_test/fdt_main.c

`tools/fdt_host_test/fdt_main.c` · 1 个函数 · 1 个有说明 · 25 行

<a id="fn-tools-fdt_host_test-fdt_main.c-10"></a>

### `main`

```c
int main(int argc, char **argv)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读入 DTB 并定位 no-map 偏移，调用 host_patch_all 打补丁后再次定位并打印前后结果，最后把修改后的镜像写入 argv[2]。

**位置**：`tools/fdt_host_test/fdt_main.c:10`

<a id="mod-tools-fdt_host_test-fdtshim-cache.h"></a>

## tools/fdt_host_test/fdtshim/cache.h

`tools/fdt_host_test/fdtshim/cache.h` · 1 个函数 · 1 个有说明 · 12 行

<a id="fn-tools-fdt_host_test-fdtshim-cache.h-10"></a>

### `clean_dcache_range`

```c
static inline void clean_dcache_range(const void *addr, size_t size)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

空实现，忽略参数，供宿主测试环境屏蔽真实缓存清理操作。

**位置**：`tools/fdt_host_test/fdtshim/cache.h:10`
