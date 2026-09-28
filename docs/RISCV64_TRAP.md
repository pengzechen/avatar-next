• 结论

  当前 RISC-V 已经具备“从内核态进入 trap 后不破坏通用寄存器”的基础，真正阻止内核态接收中断的不是入口保存现场，而是 C 层的调度、锁和
  中断上下文约束还没准备好。

  不能直接把 boot/riscv64/exception.c:55 里的 CSR_SET(sstatus, SSTATUS_SIE) 打开。那样 timer 会在任意内核代码中触发，随后 boot/
  riscv64/exception.S:137 每次 trap 出口都会调用 sched_check_and_yield()，而 kernel/task/sched.c:328 会在 need_resched 时直接进入
  sched_schedule()。这会让内核代码在持锁、修改链表、执行 syscall 中途被抢占，风险很高。

  现状梳理

  RISC-V trap 入口：

  boot/riscv64/exception.S:23 使用 sscratch=0 区分 S 态和 U 态来源：

  - sscratch == 0：S→S，说明从内核态陷入。
  - sscratch != 0：U→S，sscratch 保存用户任务的 kernel stack top。

  S→S 路径在 boot/riscv64/exception.S:38 会直接在当前内核栈上减 TRAP_FRAME_SIZE，并保存完整 x0..x31 + sepc/scause/stval/sstatus。这
  说明现在的汇编入口已经可以承受内核态中断的基础现场保存。

  U→S 路径在 boot/riscv64/exception.S:49 切到 sscratch 指定的内核栈，再保存完整 trap frame。返回 U 态前又在 boot/riscv64/
  exception.S:153 根据 SPP 重新设置 sscratch。

  当前禁用点：

  boot/riscv64/exception.c:43 明确说不启用 sstatus.SIE，也就是说：

  - sie.STIE 会在 driver/timer/timer_riscv64_impl.h:66 打开。
  - 但 S-mode 全局中断 sstatus.SIE 在内核里保持 0。
  - 用户态通过 sret 的 SPIE -> SIE 机制启用中断，所以 timer 主要在用户态运行时生效。

  timer 路径：

  driver/timer/timer_riscv64_impl.h:127 的 timer_handler() 会：

  - 调 signal_check_uart()。
  - 更新 tick 统计。
  - timer_schedule_next_tick() 重新设置 SBI timer。
  - 调 g_tick_cb()，目前调度器注册的是 sched_tick()。

  sched_tick() 在 kernel/task/sched.c:295 只设置 cpu_current()->need_resched = true，没有在 ISR 里直接切换。真正切换发生在 trap 出口
  boot/riscv64/exception.S:137 调 sched_check_and_yield() 时。

  这点是好事：已经是“ISR 只置位，出口统一调度”的结构。

  核心风险

  1. sched_check_and_yield() 不区分用户态/内核态

  现在 kernel/task/sched.c:323 的注释明确说“不再限制仅用户进程可被抢占”。如果打开内核态中断，timer 在 S-mode 触发后，trap 出口会照样
  调度。

  这意味着 syscall、内核线程、文件系统、内存管理、驱动路径都可能在任意点被抢占。

  2. arch_irq_save/restore 只管硬件 SIE，没有 preempt nesting

  kernel/task/switch.h:134 的 RISC-V arch_irq_save() 只是清 sstatus.SIE，没有记录“当前不允许抢占”的嵌套深度。这样调度器无法判断：

  - 当前是否在中断上下文。
  - 当前是否在 irqsave 临界区。
  - 当前是否允许内核抢占。

  3. 普通 spinlock_t 在内核中断打开后可能自死锁

  例如：

  - kernel/mm/pmm.c:82 用普通 spin_lock(&pmm->lock)。
  - driver/ion/ion.c:76 用普通 spin_lock(&g_ion_lock)。
  - UART 部分代码也用普通 spinlock_t。

  如果普通内核路径持有 spinlock_t 时被 timer/外设中断打断，而中断处理路径又间接碰到同一资源，就会在同一 CPU 上自旋等待自己释放锁。

  4. spinlock_noirq_t 目前只有一个 irq_flags 字段，不能安全嵌套/并发

  include/spinlock.h:21 的 spinlock_noirq_t 把 irq_flags 存在锁对象里。单 CPU、非嵌套时还能工作；但如果同一把锁被不同上下文竞争，或
  者将来 SMP，锁对象里的 flags 会互相覆盖。更合理的 API 是 flags 由调用者栈变量保存，像 Linux 的 spin_lock_irqsave(lock, flags)。

  短期可以不大改接口，但设计上必须知道这是后续隐患。

  5. timer handler 里做的事情偏多

  driver/timer/timer_riscv64_impl.h:132 每 tick 调 signal_check_uart()。内核态中断打开后，这个函数会在任意内核位置运行。如果它触碰
  TTY、进程、信号、锁或文件描述符结构，就扩大了 ISR 可重入面。

  第一阶段最好让 timer ISR 尽量短：更新时间、重装 timer、置 need_resched，把 UART/signal 检查延迟到安全点或软中断/底半部。

  推荐方案：分两阶段做

  阶段 1：允许内核态接收中断，但不允许随意抢占内核

  目标是先证明 RISC-V S-mode 能收到 timer interrupt，并且不会破坏内核现场。这个阶段只做“中断可进入”，不做“内核任意点可抢占”。

  规则：

  - 用户态 timer 中断后，允许调度。
  - 内核态 timer 中断后，只处理 timer、置 need_resched，返回原内核执行点。
  - 只有在明确安全点才执行调度，例如 syscall 返回用户态前、主动 task_yield()、idle loop。

  实现建议：

  1. 在 trap frame 判断来源模式

  frame->sstatus & SSTATUS_SPP：

  - SPP=0：trap 前是 U-mode。
  - SPP=1：trap 前是 S-mode。

  在 C 或汇编层提供 helper：

  static inline bool trap_from_kernel(const trap_frame_t *frame)
  {
      return (frame->sstatus & SSTATUS_SPP) != 0;
  }

  2. 修改 trap 出口调度策略

  现在 boot/riscv64/exception.S:137 无条件调用 sched_check_and_yield()。建议改成传入 trap_frame_t *：

  mv    a0, sp
  call  sched_check_and_yield_from_trap

  新增：

  bool sched_check_and_yield_from_trap(trap_frame_t *frame)
  {
      bool from_kernel = frame->sstatus & SSTATUS_SPP;

      if (from_kernel) {
          /*
           * 阶段 1：内核态中断只置 need_resched，不在 trap 出口抢占内核。
           * 避免在锁、链表、syscall 中间切走。
           */
          return false;
      }

      return sched_check_and_yield();
  }

  这样打开 SIE 后，内核能收到中断，但不会在内核中断返回前调度走。

  3. 在 exception_init() 不要立刻开 SIE，新增显式开关函数

  不要把 exception_init() 改成自动开中断。当前内核初始化早期很多代码默认不可中断。建议新增：

  void riscv_kernel_interrupt_enable(void)
  {
      CSR_SET(sstatus, SSTATUS_SIE);
  }

  void riscv_kernel_interrupt_disable(void)
  {
      CSR_CLEAR(sstatus, SSTATUS_SIE);
  }

  然后在 kernel/main.c:196 的 timer_set_tick_cb(sched_tick) 之后、系统任务和锁初始化完成后，再打开：

  timer_set_tick_cb(sched_tick);
  #if ARCH_RISCV64
  riscv_kernel_interrupt_enable();
  #endif

  注意：如果 task_trampoline() 已经会 kernel/task/task.c:171 调 arch_irq_enable()，那新内核任务首次运行时本来就会打开 SIE。问题是当
  前设计依赖“内核代码之后会通过 irqsave 再关掉/用户态机制处理”，所以需要把整体语义理清。阶段 1 应该明确：调度开始后内核任务可以
  SIE=1，但 timer 不在 S-mode trap 出口抢占。

  4. 保持 ISR 非嵌套

  RISC-V 硬件进入 trap 时会清 sstatus.SIE，保存原值到 SPIE。当前 trap handler 没有在 ISR 中重新置 SIE，所以不会发生中断嵌套。建议阶
  段 1 保持这个行为，不要在 handle_exception() 中重新开中断。

  这能大幅降低复杂度。

  5. 精简 timer ISR

  建议第一阶段把 driver/timer/timer_riscv64_impl.h:132 的 signal_check_uart() 从 timer hardirq 中移出去，或者至少加条件：

  - 如果 trap_from_kernel，不做 UART/signal 扫描。
  - 如果 trap_from_user，可以暂时保留。

  更干净的做法是 timer 只置一个 need_signal_poll 标志，后面在 syscall 返回、idle、安全点处理。

  6. 修正注释

  boot/riscv64/exception.c:45 注释说开中断会破坏 t0/t1，这已经不准确。入口汇编已经保存了完整寄存器。应该改成：当前关闭 SIE 是因为 C
  层同步和内核抢占语义未完善。

  阶段 1 的验收标准

  加临时计数器：

  volatile uint64_t g_rv_irq_from_kernel;
  volatile uint64_t g_rv_irq_from_user;

  在 handle_exception() 的 interrupt path 根据 SPP 统计。跑 busybox 或一个长 syscall/内核循环时确认：

  - g_rv_irq_from_kernel 会增长。
  - 系统不崩。
  - 内核态 timer interrupt 返回后继续原内核路径。
  - 用户态 timer interrupt 仍能驱动调度。

  阶段 2：支持内核态可抢占

  这一步才是完整的“内核态也能被中断并抢占”。需要加 preempt 语义。

  1. 在 cpu_t 中增加状态

  建议在 kernel/task/cpu.h 的 cpu_t 增加：

  uint32_t irq_depth;
  uint32_t preempt_count;

  语义：

  - irq_depth > 0：当前在硬中断上下文，不能睡眠，不能直接调度。
  - preempt_count > 0：当前处于不可抢占区域。
  - need_resched == true：需要在可抢占点调度。

  2. trap 入口/出口维护 irq_depth

  在 handle_exception() 前后维护更理想，但它只覆盖 C handler，不覆盖汇编保存/恢复。阶段 2 可以先在 handle_exception() 中对 interrupt
  path：

  if (cause & SCAUSE_INTERRUPT_BIT) {
      cpu_current()->irq_depth++;
      ...
      cpu_current()->irq_depth--;
  }

  由于硬中断不嵌套，这已经够用。

  3. arch_irq_save() 和 arch_irq_restore() 不等于 preempt disable，但可先绑定

  短期保守策略：所有关 IRQ 临界区也禁止抢占。

  把 arch_irq_save() 的调用点迁移到包装函数会比较大。更实际的第一步是在调度检查里判断硬件 SIE 和 preempt 状态：

  bool sched_can_preempt_kernel(void)
  {
      cpu_t *c = cpu_current();
      return c->irq_depth == 0 && c->preempt_count == 0;
  }

  然后提供：

  void preempt_disable(void);
  void preempt_enable(void);

  spin_lock_irqsave()、mutex 内部、调度器关键区逐步加入。

  4. 修改 sched_check_and_yield_from_trap

  阶段 2 策略：

  bool sched_check_and_yield_from_trap(trap_frame_t *frame)
  {
      cpu_t *c = cpu_current();

      if (!c->need_resched)
          return false;

      bool from_user = !(frame->sstatus & SSTATUS_SPP);
      if (from_user) {
          c->need_resched = false;
          sched_schedule();
          return true;
      }

      /*
       * 内核态抢占只在明确允许时发生。
       */
      if (c->irq_depth == 0 && c->preempt_count == 0) {
          c->need_resched = false;
          sched_schedule();
          return true;
      }

      return false;
  }

  但注意：如果这个函数是在硬中断 trap 出口调用，此时 irq_depth 应该已经减回 0。否则它永远不会抢占。建议结构是：

  handle_exception(frame);        // interrupt handler 内 irq_depth++/--
  sched_check_and_yield_from_trap(frame);

  也就是当前汇编顺序可以保留，只要 handle_exception() 返回前 irq_depth 已恢复。

  5. 给锁建立清晰规则

  建议后续统一：

  - 会被 hardirq 和普通内核上下文同时访问的锁：必须用 irqsave。
  - 只在进程上下文访问、不会被 ISR 触达的锁：可以普通 spinlock。
  - 会睡眠的 mutex：禁止在 hardirq 中使用。
  - task_block()、mutex_lock() 这种可能调度/睡眠的函数要断言 irq_depth == 0。

  短期需要重点审查：

  - signal_check_uart() 触达的数据结构。
  - klog/UART 输出路径是否可能在 ISR 中拿锁。
  - PMM 是否可能在 ISR 中被调用。原则上 hardirq 不该分配内存。
  - sched run queue 锁已经有 irqsave 变体，但 spinlock_noirq_t 的 flags 存在锁对象里，长期需要重构。

  6. 改造 spin_lock_irqsave API

  当前 API：

  spin_lock_irqsave(&lock);
  spin_unlock_irqrestore(&lock);

  flags 存在 lock->irq_flags，不适合嵌套/SMP。

  建议新 API：

  uint64_t flags;
  spin_lock_irqsave(&lock, &flags);
  spin_unlock_irqrestore(&lock, flags);

  或者宏风格：

  #define spin_lock_irqsave(lock, flags) \
      do { flags = arch_irq_save(); spin_lock(lock); } while (0)

  如果暂时不想大规模改，可以先新增 spin_lock_irqsave2()，逐步迁移高风险路径。

  最小可落地 patch 顺序

  1. 增加 sched_check_and_yield_from_trap(trap_frame_t *frame)，S-mode trap 暂不调度。
  2. 修改 boot/riscv64/exception.S:137，把 call sched_check_and_yield 换成传 frame 的新函数。
  3. 新增 RISC-V riscv_kernel_interrupt_enable/disable()，在调度初始化完成后打开 SIE。
  4. 给 handle_exception() 的 interrupt path 加 from_kernel/from_user 计数器。
  5. 暂时限制 timer hardirq 中的 signal_check_uart()，至少不要在 S-mode 中断里跑它。
  6. 跑 qemu-virt-riscv64，验证内核态中断计数增长。
  7. 再引入 irq_depth/preempt_count，逐步允许内核安全点抢占。
  8. 最后审计并迁移 ISR 可重入路径上的普通 spinlock。

  建议的阶段 1 关键代码形态

  exception.S：

      mv    a0, sp
      call  handle_exception

      mv    a0, sp
      call  sched_check_and_yield_from_trap

  sched.c：

  #if ARCH_RISCV64
  #include "riscv64/exception.h"

  bool sched_check_and_yield_from_trap(trap_frame_t *frame)
  {
      if (!frame)
          return sched_check_and_yield();

      if (frame->sstatus & SSTATUS_SPP) {
          /*
           * Stage 1: allow S-mode interrupts, but do not preempt arbitrary
           * kernel code from trap return yet.
           */
          return false;
      }

      return sched_check_and_yield();
  }
  #endif

  exception.c：

  volatile uint64_t g_rv_irq_from_kernel;
  volatile uint64_t g_rv_irq_from_user;

  void riscv_kernel_interrupt_enable(void)
  {
      CSR_SET(sstatus, SSTATUS_SIE);
  }

  void riscv_kernel_interrupt_disable(void)
  {
      CSR_CLEAR(sstatus, SSTATUS_SIE);
  }

  在 interrupt path：

  if (cause & SCAUSE_INTERRUPT_BIT) {
      if (frame->sstatus & SSTATUS_SPP)
          g_rv_irq_from_kernel++;
      else
          g_rv_irq_from_user++;

      ...
  }

  我不建议的做法

  不要直接在 exception_init() 里打开 sstatus.SIE 并保留当前无条件 sched_check_and_yield()。这会把“内核态能接收中断”和“内核任意点可抢
  占”混在一起，问题会随机出现在文件系统、信号、TTY、内存分配或调度链表上，很难定位。

  不要在 hardirq 中重新打开 SIE 做中断嵌套。当前目标只需要内核态接收中断，不需要 nested interrupt。嵌套会立刻要求 per-CPU irq
  stack、锁分层、ISR 重入审计，复杂度会翻倍。

  不要依赖“单核所以没事”。单核打开内核态中断后，同一 CPU 的中断上下文和被打断的内核上下文仍然会重入共享数据结构，普通 spinlock 一样
  可能自死锁。

  推荐最终语义

  长期目标应该是：

  - RISC-V 内核任务默认 SIE=1，可以接收 timer/external interrupt。
  - hardirq 入口不嵌套，handler 尽量短。
  - timer hardirq 只更新时间、重装 timer、置 resched/softirq 标志。
  - 用户态 trap 返回前可直接调度。
  - 内核态 trap 返回前只在 preempt_count == 0 && irq_depth == 0 时调度。
  - 所有 irqsave 临界区隐含不可抢占。
  - 所有可能被 ISR 触达的数据结构使用 irq-safe 锁。
  - 会睡眠的锁和函数在 irq_depth > 0 时直接 assert/panic，避免 silent corruption。

  这样可以先快速拿到“内核态能收中断”的能力，又不会一次性把整个内核推进完全可抢占内核的复杂度里。