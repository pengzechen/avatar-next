# Spinlock (spinlock.h)

## Overview

Spinlocks provide mutual exclusion using busy-waiting. Suitable for short critical sections in kernel code.

## Data Structures

```c
typedef struct {
    volatile uint32_t lock;
} spinlock_t;

typedef struct {
    volatile uint32_t lock;
} spinlock_noirq_t;
```

## API

### Basic Operations

```c
spin_init(&lock)                    // Initialize spinlock
spin_lock(&lock)                    // Acquire lock
spin_unlock(&lock)                  // Release lock
spin_trylock(&lock)                 // Try to acquire (non-blocking)
spin_is_locked(&lock)               // Check if locked
```

### Interrupt-Safe Operations

```c
spin_lock_irqsave(&lock, flags)     // Acquire with interrupt disable
spin_unlock_irqrestore(&lock, flags) // Release and restore interrupts
```

## Usage Example

```c
spinlock_t my_lock = SPINLOCK_INIT;

void critical_section(void)
{
    spin_lock(&my_lock);
    // Critical section
    shared_data++;
    spin_unlock(&my_lock);
}
```

## Interrupt Context

**Important**: In interrupt handlers, always use `irqsave` variant:

```c
void interrupt_handler(void)
{
    unsigned long flags;
    spin_lock_irqsave(&lock, flags);
    // Safe to access shared data
    spin_unlock_irqrestore(&lock, flags);
}
```

## Architecture Implementations

### AArch64

Uses **LDAXR/STLXR** (Load-Acquire/Store-Release Exclusive):
- `ldaxr` - Load with acquire semantics
- `stxr` - Store with release semantics
- `wfe` - Wait For Event (power optimization)

### RISC-V

Uses **LR/SC** (Load-Reserved/Store-Conditional):
- `lr.w` - Load-Reserved, marks memory location
- `sc.w` - Store-Conditional, succeeds only if location unchanged

### x86_64

Uses **lock xchg**:
- `xchg` - Atomic exchange
- `lock` prefix - Ensures atomicity and memory ordering

## Best Practices

1. **Keep critical sections short** - Other CPUs spin waiting
2. **No sleeping in critical section** - No blocking calls
3. **Use irqsave in interrupt context** - Prevents deadlock
4. **Unlock in reverse order** - If acquiring multiple locks

## Common Pitfalls

```c
// ❌ WRONG - Can cause deadlock
void interrupt_handler(void) {
    spin_lock(&lock);  // If interrupted while holding lock → DEADLOCK
}

// ✅ CORRECT
void interrupt_handler(void) {
    unsigned long flags;
    spin_lock_irqsave(&lock, flags);
    // ...
    spin_unlock_irqrestore(&lock, flags);
}
```

## Performance Considerations

- Spinlocks waste CPU cycles while waiting
- Use for short critical sections (< few microseconds)
- For longer waits, consider sleepable locks (future work)

## See Also

- `include/spinlock.h` - Main header
- `include/aarch64/spin_lock_impl.h` - AArch64 implementation
- `include/riscv64/spin_lock_impl.h` - RISC-V implementation
- `include/x86_64/spin_lock_impl.h` - x86_64 implementation
- `docs/BARRIER.md` - Memory barrier documentation
