# MMIO 操作与内存屏障

## 概述

为 MMIO（Memory-Mapped I/O）操作添加了完整的内存屏障支持，确保设备寄存器访问的正确性和可预测性。

## API 分类

### 1. Relaxed 版本（无屏障）

适用于性能关键且已知不需要内存屏障的场景。

```c
uint8_t  val8  = read8_relaxed(addr);
uint16_t val16 = read16_relaxed(addr);
uint32_t val32 = read32_relaxed(addr);
uint64_t val64 = read64_relaxed(addr);

write8_relaxed(val8, addr);
write16_relaxed(val16, addr);
write32_relaxed(val32, addr);
write64_relaxed(val64, addr);
```

**特性**：
- 不保证内存排序
- 不保证写入立即对设备可见
- 性能最优，但需要程序员明确知道何时可以安全使用

### 2. 完整屏障版本

默认的 `readX` / `writeX` 函数，读写前后都有完整的数据屏障。

```c
uint8_t  val8  = read8(addr);
uint16_t val16 = read16(addr);
uint32_t val32 = read32(addr);
uint64_t val64 = read64(addr);

write8(val8, addr);
write16(val16, addr);
write32(val32, addr);
write64(val64, addr);
```

**特性**：
- 读写前后都有 `barrier_data()`
- 确保操作按顺序执行
- 写入立即对设备可见

### 3. Post-Barrier 版本（仅写后屏障）

写入操作后只添加写屏障，确保写入对设备可见。

```c
write8_post(val8, addr);
write16_post(val16, addr);
write32_post(val32, addr);
write64_post(val64, addr);
```

**特性**：
- 只在写入后添加 `barrier_data_write()`
- 比完整屏障版本性能更好
- 适用于：写入后需要立即触发设备操作的场景

### 4. Linux 兼容宏

提供与 Linux 内核兼容的宏定义。

```c
/* 带屏障 */
mmio_readb/mmio_readw/mmio_readl
mmio_writeb/mmio_writew/mmio_writel

/* Relaxed */
mmio_readb_relaxed/mmio_readw_relaxed/mmio_readl_relaxed
mmio_writeb_relaxed/mmio_writew_relaxed/mmio_writel_relaxed
```

## 使用场景

### 场景 1: 设备初始化

```c
/* 写入多个寄存器来配置设备 */
write32_relaxed(config_val, ctrl_reg);   // 配置
write32_relaxed(enable_val, enable_reg);  // 使能
write32_post(trigger_val, trigger_reg);   // 触发（需要立即生效）
```

### 场景 2: 读取设备状态

```c
/* 读取状态寄存器，检查设备是否就绪 */
uint32_t status = read32(status_reg);
if (status & READY_BIT) {
    /* 设备就绪 */
}
```

### 场景 3: DMA 描述符设置

```c
/* 设置 DMA 描述符（多个写操作） */
write32_relaxed(src_addr, desc_src);
write32_relaxed(dst_addr, desc_dst);
write32_relaxed(size, desc_len);
write32_post(start_cmd, dma_ctrl);  // 启动 DMA
```

### 场景 4: 高频访问（性能敏感）

```c
/* 批量读取 FIFO（使用 relaxed 版本）*/
for (int i = 0; i < count; i++) {
    data[i] = read32_relaxed(fifo_data);
}
barrier_data();  /* 批量读取后统一屏障 */
```

## 内存屏障详解

### Read 前后屏障

```c
static inline uint32_t read32(volatile void *addr)
{
    uint32_t value;
    barrier_data();         /* 前屏障：确保之前的操作完成 */
    value = *(volatile uint32_t *)addr;
    barrier_data();         /* 后屏障：防止后续操作被重排 */
    return value;
}
```

**作用**：
- 前屏障：确保之前的所有内存操作完成
- 后屏障：确保读取完成后再执行后续操作

### Write 前后屏障

```c
static inline void write32(uint32_t value, volatile void *addr)
{
    barrier_data();         /* 前屏障：确保之前的操作完成 */
    *(volatile uint32_t *)addr = value;
    barrier_data();         /* 后屏障：确保写入对设备可见 */
}
```

**作用**：
- 前屏障：确保之前的所有内存操作完成
- 后屏障：确保写入立即对设备可见（不会延迟写入）

## 性能考虑

### 性能排序（从快到慢）

1. **Relaxed** - 无屏障，最快
2. **Post-Barrier** - 仅写后屏障
3. **Full Barrier** - 完整屏障，最慢但最安全

### 选择建议

- **设备初始化**：使用 Relaxed + Post-Barrier
- **状态查询**：使用 Full Barrier
- **批量操作**：Relaxed + 统一屏障
- **默认行为**：使用 Full Barrier（安全）

## 架构差异

虽然使用了统一的 barrier API，但底层实现有差异：

| 架构 | barrier_data() | 性能影响 |
|------|----------------|----------|
| x86_64 | mfence | 较小（TSO） |
| AArch64 | dmb ish | 较大（弱一致性） |
| RISC-V64 | fence rw,rw | 中等 |

## 编译验证

```bash
✓ make ARCH=x86_64
✓ make ARCH=aarch64
✓ make ARCH=riscv64
```

所有架构都能正常编译和使用。
