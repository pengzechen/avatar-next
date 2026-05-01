# Memory Barriers (barrier.h)

## Overview

Memory barriers prevent CPU and compiler reordering of memory operations, ensuring correct synchronization in multi-core systems.

## API

### Compiler Barrier

```c
barrier_compiler()
```
Prevents compiler reordering (no CPU instructions).

### Data Barriers

```c
barrier_data()           // Full memory barrier (mb)
barrier_data_read()      // Read barrier (rmb)
barrier_data_write()     // Write barrier (wmb)
```

### Instruction Barrier

```c
barrier_instr_full()     // Instruction sync barrier
```

### Acquire/Release Semantics

```c
barrier_acquire()        // Load-acquire
barrier_release()        // Store-release
```

## Usage Example

```c
// Producer-Consumer pattern
int data = 0;
bool ready = false;

// Producer
data = 42;
barrier_data_write();  // Ensure data is written first
ready = true;

// Consumer
if (ready) {
    barrier_data_read();  // Ensure data is read after ready check
    int value = data;     // value is guaranteed to be 42
}
```

## Architecture Implementations

| Operation | AArch64 | RISC-V | x86_64 |
|-----------|---------|--------|--------|
| Compiler | `"{} ::: "memory"` | `"{} ::: "memory"` | `"{} ::: "memory"` |
| Data | `dmb ish` | `fence rw, rw` | `mfence` |
| Read | `dmb ishld` | `fence r, r` | `lfence` |
| Write | `dmb ishst` | `fence w, w` | `sfence` |
| Instr | `isb` | `fence.i` | - |

## Important Notes

1. **Use with MMIO**: All MMIO operations include appropriate barriers
2. **Spinlocks**: Use acquire/release semantics in lock implementations
3. **Performance**: Barriers have overhead, use only when necessary

## See Also

- `include/barrier.h` - Main header
- `include/aarch64/barrier_impl.h` - AArch64 implementation
- `include/riscv64/barrier_impl.h` - RISC-V implementation
- `include/x86_64/barrier_impl.h` - x86_64 implementation
