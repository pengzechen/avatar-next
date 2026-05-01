# Cache Operations (cache.h)

## Overview

Cache management operations for maintaining memory coherence in kernel code.

## API

```c
cache_clean_range(addr, size)          // Clean cache lines to memory
cache_invalidate_range(addr, size)     // Invalidate cache lines
cache_clean_and_invalidate_range(addr, size)  // Clean and invalidate
```

## Usage Examples

### DMA Operations

```c
// Before DMA read: invalidate cache
cache_invalidate_range(dma_buffer, size);
start_dma_read(dma_buffer);

// After DMA write: clean cache
cache_clean_range(dma_buffer, size);
start_dma_write(dma_buffer);
```

### Memory-Mapped I/O

```c
// Ensure device sees latest data
cache_clean_range(device_buffer, size);

// Invalidate after device updates memory
cache_invalidate_range(device_buffer, size);
```

### Code Modification

```c
// Before jumping to dynamically loaded code
cache_clean_and_invalidate_range(code_addr, code_size);
flush_instruction_cache();
```

## Architecture Implementations

### AArch64

Uses **DC (Data Cache)** instructions:
- `dc cvac` - Clean data cache line to point of coherency
- `dc ivac` - Invalidate data cache line
- Combined: clean then invalidate

### RISC-V

Uses **CBO (Cache Block Operations)**:
- `cbo.clean` - Clean cache block
- `cbo.inval` - Invalidate cache block
- Requires Zicbom extension

### x86_64

Uses **CLFLUSH** instructions:
- `clflush` - Flush cache line
- `clflushopt` - Optimized flush (with memory tracking)
- Combined: flush then invalidate (implicit)

## Alignment Considerations

Cache operations work on cache line boundaries:
- Typical cache line size: 64 bytes
- API handles unaligned addresses automatically
- For performance, align data to cache line size

```c
#define CACHE_LINE_SIZE 64

// Align buffer to cache line
char buffer[1024] __attribute__((aligned(CACHE_LINE_SIZE)));
```

## Performance Impact

Cache operations have overhead:
- **Clean**: ~50-100 cycles per cache line
- **Invalidate**: ~50-100 cycles per cache line
- **Clean+Invalidate**: ~100-200 cycles per cache line

**Best Practices**:
- Only invalidate what's necessary
- Batch operations when possible
- Use DMA coherency if hardware supports it

## Common Pitfalls

```c
// ❌ WRONG - Forgetting to invalidate before DMA read
start_dma_read(buffer);
// CPU may read stale data from cache

// ✅ CORRECT
cache_invalidate_range(buffer, size);
start_dma_read(buffer);
```

```c
// ❌ WRONG - Forgetting to clean before DMA write
memcpy(buffer, data, size);
start_dma_write(buffer);
// Device may read stale data from RAM

// ✅ CORRECT
cache_clean_range(buffer, size);
start_dma_write(buffer);
```

## See Also

- `include/cache.h` - Main header
- `include/aarch64/cache_impl.h` - AArch64 implementation
- `include/riscv64/cache_impl.h` - RISC-V implementation
- `include/x86_64/cache_impl.h` - x86_64 implementation
- `docs/BARRIER.md` - Memory barrier documentation
