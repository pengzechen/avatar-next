/* driver/npu/rknpu.c

   Rockchip RK3588 NPU 驱动

   初始化流程：
     1. 通过 rkpm 上电四个 NPU 电源域
     2. 清除遗留中断状态
     3. 读取并验证版本寄存器(魔数 0x46495245 = "FIRE")
     4. 向 GICv3 注册 NPU0 IRQ 处理程序并使能中断

   中断处理：
     读本核 INT_STATUS → 与期望的 fuzz 掩码比对 → 记下 PC_TASK_STATUS 的完成数
     → 清本核 INT_CLEAR。三个核各有独立中断线(142/143/144)。

   框架集成：
     - 通过 platform_get_mmio("npu", "base0") 获取 MMIO 基地址
       (不能用 platform_get_uintptr —— 那是裸物理地址, 见 rknpu_init 里的说明)
     - 通过 platform_get_uintptr("npu", "irq0") 获取中断号(中断号不是地址)
     - 由平台 C 初始化序列在 drivers 阶段调用
*/

#include "rknpu.h"
#include "npu/npu_mmu.h"
#include "rkpm.h"
#include "rknpu_drm.h" /* struct rknpu_submit(用户态 ABI) */

#include "types.h"
#include "mmio.h"
#include "mm_vm.h"   /* KERNEL_VMA(硬编码 MMIO 回退地址要按平台规则换算) */
#include "dma_buf.h" /* 排障：按物理地址反查缓冲区, dump regcmd */
#include "klog.h"
#include "platform_cfg.h"
#include "exception.h"
#include "irq/gicv3.h"
#include "spinlock.h"
#include "timer/timer.h"

/* ── 运行时状态 ──────────────────────────────────────────────────────────── */

/*
   每个核一份：MMIO 基址, 中断号, IRQ 计数。

   三个核是并列的独立设备 —— 各自的 MMIO 窗口, 各自的中断线, 各自跑自己的
   任务片。只有 PC 侧寄存器偏移是共用的(核内相对偏移相同)。
*/
static uintptr_t g_npu_base[RKNPU_MAX_CORES];
static int g_npu_irq[RKNPU_MAX_CORES];
static volatile int g_irq_count[RKNPU_MAX_CORES];
/*
   ISR 在中断那一刻读到的本核 INT_STATUS 原值。

   为什么不报"这个核跑了几个任务"：这个数拿不到。实测两条路都不通 ——
     - PC_TASK_STATUS(0x3C)：作业完成时被硬件清零, 中断时刻读必然是 0
       (它只在作业跑到一半时有意义, 比如卡死那次读到的 completed=44)；
     - INT_STATUS(0x28) 的低 12 位不是计数, 是状态位(实测 1024/2048 =
       0x400/0x800), 我一度把它当计数器用, 打出过"2 个任务完成 1024 个"。

   所以报真的读得到的东西：本核 INT_STATUS 的原值 + 它是否匹配期望掩码。
   见 rknpu_irq_common() 里的比对。
*/
static volatile uint32_t g_core_status[RKNPU_MAX_CORES];
/*
   "本核第一次收到中断"标志 —— 一次启动只报一次, 3 行, 用来直接看到
   三个核各自的中断线都响了。不能直接用 KLOG_*_ONCE：那是按调用点去重的,
   而这里三个核走的是同一个函数, 用 ONCE 只会报第一个核。
*/
static bool g_irq_seen[RKNPU_MAX_CORES];
/* platform.conf 里真正配了基址的核数(没配的核基址是 0) */
static int g_npu_cores = 0;
/* 初始化完成前拒绝提交(提交会写寄存器, 基址可能是 0) */
static bool g_npu_ready = false;
static bool g_rknpu_regcmd_dumped; /* 见 rknpu_job_commit 里的说明 */

/* ── RK3588 NPU 配置 ─────────────────────────────────────────────────────── */
static const rknpu_config_t rk3588_cfg = {
    .pc_data_amount_scale = 2,
    .pc_task_number_bits = 12,
    .pc_task_number_mask = 0xFFF,
    .pc_task_status_offset = 0x3C,
    .core_mask = 0x7, /* 三核全启 */
};

/* ── RK3588 CRU：时钟门控(只读诊断)─────────────────────────────────────

   为什么是只读的：参考项目 testos-reflector 的 `enable_rk3588_npu_clocks()`
   在整个仓库里没有任何调用者, 而它的 NPU 用例是真机跑通的 —— 说明
   NPU 时钟在它们的启动链里(U-Boot/BL31)就已经开着了。所以我们先只读
   不写：把 gate 状态打出来, 就能判断"时钟没开"是不是本次超时的原因,
   而不用先去实现一整套 CRU 时钟树。

   寄存器布局见参考项目 include/dev/cru.h：
       CLKSEL_CON(n)  = CRU + 0x300 + n*4
       CLKGATE_CON(n) = CRU + 0x800 + n*4   (bit[31:16]=写掩码, bit=0 表示开)
       SOFTRST_CON(n) = CRU + 0xa00 + n*4
   rk3588 的 mmio_vma=true, 所以这里同样要按平台规则换算(否则读到的是
   没映射的物理地址, 症状又是一次莫名其妙的翻译错误)。
*/
#define RKNPU_CRU_BASE       0xFD7C0000UL
#define RKNPU_CLKSEL_CON(n)  (RKNPU_CRU_BASE + 0x300U + (n) * 4U)
#define RKNPU_CLKGATE_CON(n) (RKNPU_CRU_BASE + 0x800U + (n) * 4U)
#define RKNPU_SOFTRST_CON(n) (RKNPU_CRU_BASE + 0xa00U + (n) * 4U)

static uintptr_t cru_xlate(uintptr_t pa)
{
    return platform_mmio_needs_vma() ? pa + KERNEL_VMA : pa;
}

static inline uint32_t cru_read(uintptr_t pa)
{
    return read32((void *)cru_xlate(pa));
}

/* 一次性把 NPU 相关 gate/reset/mux 打出来(只读, 不改任何状态) */
static void rknpu_dump_cru(void)
{
    uint32_t g29 = cru_read(RKNPU_CLKGATE_CON(29));
    uint32_t g30 = cru_read(RKNPU_CLKGATE_CON(30));
    uint32_t g27 = cru_read(RKNPU_CLKGATE_CON(27));
    uint32_t g28 = cru_read(RKNPU_CLKGATE_CON(28));
    uint32_t rst30 = cru_read(RKNPU_SOFTRST_CON(30));
    uint32_t sel73 = cru_read(RKNPU_CLKSEL_CON(73));
    uint32_t sel74 = cru_read(RKNPU_CLKSEL_CON(74));

    KLOG_INFO("rknpu: CRU GATE29=0x%08x GATE30=0x%08x GATE27=0x%08x "
              "GATE28=0x%08x\n",
              g29, g30, g27, g28);
    KLOG_INFO("rknpu: CRU SOFTRST30=0x%08x SEL73=0x%08x SEL74=0x%08x\n", rst30, sel73, sel74);

    /*
       逐条判定。0 = 时钟开 / 已出复位, 1 = 关着 / 还在复位。
       bit 位置来自参考项目 src/dev/cru.c。
    */
    KLOG_INFO("rknpu:   clk_npu_dsu0 gate=%u reset=%u mux=%u%s\n", (g29 >> 1) & 1U, rst30 & 1U, (sel73 >> 7) & 7U,
              ((g29 >> 1) & 1U) ? "  <-- core clock is gated off!" : "");
    KLOG_INFO("rknpu:   aclk_npu0 gate=%u hclk_npu0 gate=%u%s\n", (g30 >> 6) & 1U, (g30 >> 8) & 1U,
              (((g30 >> 6) & 1U) || ((g30 >> 8) & 1U)) ? "  <-- AXI/AHB clock is gated off!" : "");
    KLOG_INFO("rknpu:   pclk_npu_root gate=%u (used for register access)\n", (g29 >> 4) & 1U);
}

/* ── 内联 MMIO 辅助(第一个参数是核号)─────────────────────────────── */
static inline uint32_t npu_read(int core, uint32_t off)
{
    return read32((void *)(g_npu_base[core] + off));
}

static inline void npu_write(int core, uint32_t val, uint32_t off)
{
    write32(val, (void *)(g_npu_base[core] + off));
}

/* ── 完成判定 ────────────────────────────────────────────────────────────── */

/*
   rknpu_fuzz_status —— 把状态按 2 位一组取饱和：每对位里只要有一个非零,
   就把这对位整体置满, 其余位丢掉。

   为什么需要它：我们比的是"哪些块报了中断", 不是状态的原始位。低 12 位里
   含有会随作业进度变化的部分, 直接拿 status 去比 int_mask 会永远不等 ——
   症状是"作业明显跑完了, 却报 invalid irq status", 日志完全不指向真因。
   按 2 位一组取饱和就等于只看"这一组有没有报", 把变化的部分吸收掉。

   与参考项目 testos-reflector/src/npu/rknpu.c 的实现逐行一致(真机验证过)。

   注意： 参考项目注释里说低 12 位是"任务计数" —— 这个说法在本内核上没有验证过,
      而且和实测矛盾：INT_MASK 只放行 bit8/9, 若 status 真带 bit10/11, fuzz 会
      变成 0xF00, 与 expect=0x300 不等而报错, 但真机上从不报错。所以别把低 12 位
      当计数器用(要用完成数请读 PC_TASK_STATUS = 0x3C)。
*/
static inline uint32_t rknpu_fuzz_status(uint32_t status)
{
    uint32_t fuzz = 0;

    if ((status & 0x3) != 0)
        fuzz |= 0x3;
    if ((status & 0xc) != 0)
        fuzz |= 0xc;
    if ((status & 0x30) != 0)
        fuzz |= 0x30;
    if ((status & 0xc0) != 0)
        fuzz |= 0xc0;
    if ((status & 0x300) != 0)
        fuzz |= 0x300;
    if ((status & 0xc00) != 0)
        fuzz |= 0xc00;

    return fuzz;
}

/*
   作业完成状态 —— 每核一份。ISR 写, 提交方读, 所以都要 volatile。

     g_core_state[c]:  0 = 未完成(提交前清), 1 = 完成, -1 = 状态不匹配
     g_core_expect[c]: 该核本次期望的 fuzz 值

   为什么每核一份 expect：多核提交时每个核跑的是不同的任务片, 而期望的
   中断掩码取自该片最后一个任务的 int_mask(vendor 的
   `job->int_mask[core_index] = last_task->int_mask`)。三片可以各不相同,
   共用一个 expect 会让先完成的那颗核把后完成的判成"状态不匹配"。

   用 spinlock 而不是原子量保护状态迁移：提交方和 ISR 可能在不同核上,
   而"写 expect / 清 state / 启动 NPU"这三步必须作为一个整体, 否则
   NPU 跑得太快时中断会落在"expect 还没写"的窗口里, 判成不匹配。
*/
static volatile int g_core_state[RKNPU_MAX_CORES];
static volatile uint32_t g_core_expect[RKNPU_MAX_CORES];
static spinlock_t g_job_lock = SPINLOCK_INIT;

/* ── 中断处理程序 ────────────────────────────────────────────────────────── */

/*
   三个核的中断线是独立的(142/143/144), 各自的 ISR 只处理自己的核。
   irq_install() 的签名是 void(*)(uint64_t *frame), frame 里没有"是哪条中断"
   的信息, 所以只能写三个薄包装 —— 这也是 vendor 的做法
   (rknpu_core0/1/2_irq_handler 三个函数都转调 rknpu_irq_handler(..., core))。
*/
static void rknpu_irq_common(int core)
{
    uint32_t status = npu_read(core, RKNPU_INT_STATUS);
    /* 注意： INT_STATUS 的低 12 位是状态位, 不是完成计数 —— 别当计数器用。
          (想拿完成数得读 PC_TASK_STATUS=0x3C, 但它作业一完成就被硬件清零,
          只有作业卡住时才有值。详见 g_core_status 的说明。)
    */
    uint32_t int_lo12 = status & rk3588_cfg.pc_task_number_mask;
    uint32_t fuzz = rknpu_fuzz_status(status);

    g_irq_count[core]++;
    /* 存本核 INT_STATUS 原值 —— 这是"这个核报了中断"的直接证据。
       不报任务完成数：那个数在作业完成后读不到, 理由见 g_core_status 的说明。
    */
    g_core_status[core] = status;

    /*
       每个核第一次收到中断时报一行(一次启动共 3 行)。

       这是"三个核都在工作"最直白的证据：中断号是那个核专属的 GIC SPI
       (142/143/144), 跑的是它自己的 handler, status 是从它自己的
       INT_STATUS 读的。core1/core2 这两行出现, 就说明它们各自跑完了活,
       拉起了自己的中断线 —— 而不是"我们写了寄存器, 硬件其实没动"。
    */
    if (!g_irq_seen[core]) {
        g_irq_seen[core] = true;
        KLOG_INFO("[rknpu] core%d first interrupt: IRQ %d, status=0x%x\n", core, g_npu_irq[core], status);
    }

    if (fuzz != g_core_expect[core]) {
        g_core_state[core] = -1;
        /* 不匹配才打：这是真异常, 且要看清 raw 与计数 */
        KLOG_ERROR("[rknpu] core%d invalid irq status: 0x%x (fuzz=0x%x "
                   "expect=0x%x)\n",
                   core, status, fuzz, g_core_expect[core]);
        KLOG_ERROR("[rknpu]   core%d raw=0x%x task_counter=%u\n", core, npu_read(core, RKNPU_INT_RAW_STATUS),
                   npu_read(core, RKNPU_PC_TASK_STATUS) & rk3588_cfg.pc_task_number_mask);
    } else {
        g_core_state[core] = 1;
        KLOG_MODULE_DEBUG_SAMPLE(LOG_MODULE_DRIVER,
                                 "rknpu: core%d IRQ #%d status=0x%x "
                                 "int_status_lo12=0x%x\n",
                                 core, g_irq_count[core], status, int_lo12);
    }

    /* 清除本核的中断位 */
    npu_write(core, RKNPU_INT_CLEAR_ALL, RKNPU_INT_CLEAR);

    /* 通知 GICv3 完成(EOI 由异常框架在 irq_handler 返回后写入) */
}

static void rknpu_irq_handler0(uint64_t *frame)
{
    (void)frame;
    rknpu_irq_common(0);
}

static void rknpu_irq_handler1(uint64_t *frame)
{
    (void)frame;
    rknpu_irq_common(1);
}

static void rknpu_irq_handler2(uint64_t *frame)
{
    (void)frame;
    rknpu_irq_common(2);
}

static void (*const rknpu_irq_handlers[RKNPU_MAX_CORES])(uint64_t *) = {
    rknpu_irq_handler0,
    rknpu_irq_handler1,
    rknpu_irq_handler2,
};

/* ── 版本校验 ────────────────────────────────────────────────────────────── */
bool rknpu_validate_version(int core)
{
    uint32_t version = npu_read(core, RKNPU_VERSION);
    uint32_t version_num = npu_read(core, RKNPU_VERSION_NUM);

    KLOG_INFO("rknpu: core%d version=0x%08x version_num=0x%x\n", core, version, version_num);

    if (version != RKNPU_VERSION_MAGIC) {
        KLOG_ERROR("rknpu: core%d unexpected version 0x%08x (expected 0x%08x)\n", core, version, RKNPU_VERSION_MAGIC);
        return false;
    }

    return true;
}

/* ── 主初始化入口 ────────────────────────────────────────────────────────── */
void rknpu_init(void)
{
    KLOG_INFO("rknpu: initializing RK3588 NPU\n");

    /* 1. 从平台配置获取运行时参数 */
    /*
       注意： 必须用 platform_get_mmio(= 原始值 + KERNEL_VMA, 当 platform.conf
          的 mmio_vma 为真时), 不能用 platform_get_uintptr 拿裸物理地址。

       踩过的坑(rk3588 真机, 2026-10)：init 阶段读版本寄存器一切正常,
       因为 boot 页表里有 MMIO 的恒等映射；等 busybox 跑起来, CPU 切到
       用户进程的页表(create_uvm 建的)之后恒等映射就没了, 于是第一次
       SUBMIT 写 PC_DATA_ADDR 直接
           EL1 exception EC=0x25 ESR=0x96000045 FAR=0xfdab0010
           DFSC=0x5 (translation fault)
       ——同一个 4 KiB 页, init 能读, submit 写就炸, 看着完全不像缺映射。
       QEMU 上测不出来：那里 npu.base0 为 0, 本函数早就 return 了。

       GICv3 一直是对的(用的就是 platform_get_mmio), 照它来。
    */
    static const char *const base_keys[RKNPU_MAX_CORES] = { "base0", "base1", "base2" };
    static const char *const irq_keys[RKNPU_MAX_CORES] = { "irq0", "irq1", "irq2" };

    g_npu_cores = 0;
    for (int i = 0; i < RKNPU_MAX_CORES; i++) {
        g_npu_base[i] = platform_get_mmio("npu", base_keys[i]);
        g_npu_irq[i] = (int)platform_get_uintptr("npu", irq_keys[i]);
        if (g_npu_base[i] != 0)
            g_npu_cores++;
    }

    if (g_npu_cores == 0) {
        KLOG_ERROR("rknpu: platform 'npu.base0' not set, aborting\n");
        return;
    }
    KLOG_INFO("rknpu: %d core(s) from platform config\n", g_npu_cores);
    for (int i = 0; i < RKNPU_MAX_CORES; i++) {
        if (g_npu_base[i] != 0)
            KLOG_INFO("rknpu:   core%d base=0x%lx irq=%d\n", i, (unsigned long)g_npu_base[i], g_npu_irq[i]);
    }

    /* 2. 上电 NPU 电源域 */
    rkpm_t pm;
    /* 同样要走 VMA —— 理由见上面 base0 那段 */
    uintptr_t pmu_base = platform_get_mmio("npu", "pmu");
    if (pmu_base == 0) {
        /* 回退到硬编码地址；它也是物理地址, 同样要按平台规则换算 */
        pmu_base = platform_mmio_needs_vma() ? RKNPU_PMU1_BASE + KERNEL_VMA : RKNPU_PMU1_BASE;
    }

    rkpm_init(&pm, pmu_base);

    int ret;
    ret = rkpm_power_on(&pm, PD_NPU);
    if (ret)
        KLOG_WARN("rknpu: PD_NPU power-on timeout\n");

    ret = rkpm_power_on(&pm, PD_NPUTOP);
    if (ret)
        KLOG_WARN("rknpu: PD_NPUTOP power-on timeout\n");

    ret = rkpm_power_on(&pm, PD_NPU1);
    if (ret)
        KLOG_WARN("rknpu: PD_NPU1 power-on timeout\n");

    ret = rkpm_power_on(&pm, PD_NPU2);
    if (ret)
        KLOG_WARN("rknpu: PD_NPU2 power-on timeout\n");

    KLOG_INFO("rknpu: NPU power domains on\n");

    /* 3. 逐核：清遗留中断 + 校验版本 + 注册中断 */
    for (int i = 0; i < RKNPU_MAX_CORES; i++) {
        if (g_npu_base[i] == 0)
            continue;

        npu_write(i, RKNPU_INT_CLEAR_ALL, RKNPU_INT_CLEAR);

        if (!rknpu_validate_version(i))
            KLOG_WARN("rknpu: core%d version check failed - this core is unusable\n", i);

        if (g_npu_irq[i] > 0) {
            irq_install(g_npu_irq[i], rknpu_irq_handlers[i]);
            gicv3_enable_int(g_npu_irq[i], true);
            KLOG_INFO("rknpu: core%d IRQ %d registered and enabled\n", i, g_npu_irq[i]);
        }
    }

    /* 3b. 只读地看一眼 NPU 时钟门控 —— 版本寄存器读得出来只证明 APB 通,
           "作业不启动"最像的就是核心/AXI 时钟还关着。见 rknpu_dump_cru()。
           三个核共用一组门控, 看一次就够。
    */
    rknpu_dump_cru();

    /* 记下 NPU IOMMU 的四组寄存器位置。只记位置, 不开翻译 ——
       宿主自己驱动 NPU 时走的是物理地址, 翻译必须关着；
       等 guest 接管时 vnpu_init 才调 npu_mmu_enable()。
    */
    (void)npu_mmu_init();

    g_npu_ready = true;
    KLOG_INFO("rknpu: init done\n");
}

/*
   rknpu_host_release —— 把 NPU 让给 guest(VMM 透传用)。

   为什么必须做：本驱动在 boot 时装了 142/143/144 的 ISR 并在 GIC 里使能
   了它们。guest 提交作业 → NPU 拉高物理线 → 那条线是宿主的 GIC 在监视,
   于是宿主的 ISR 抢先跑掉, 而它的正常收尾就是写 INT_CLEAR —— guest 的中断
   还没进 vGIC 就被擦掉了。症状是 guest 里驱动一路 probe 正常, 任务提交也
   正常, 然后永远等不到中断直到超时, 且日志里一行错误都没有。

   这是"两个驱动等同一个中断"的经典局面, 谁先 ack 谁赢, 没有仲裁。
   所以让者主动退出：关掉宿主 GIC 上这三条线(线还挂着, 只是不再投递给
   宿主 CPU), 再把寄存器留给 guest。中断之后由 VMM 在每次进入 guest 前
   轮询 INT_STATUS 并按电平语义注入 vGIC(见 vmm_vnpu.h / el2_run.c)。

   g_npu_ready 也一并置回 false：宿主侧 rknpu_submit_task 从此拒绝作业。
   这不是保守 —— guest 在写同一组寄存器, 两边同时提交必然是互相破坏,
   而拒绝是可诊断的, 静默破坏不是。

   幂等；未初始化(无 NPU)时是空操作。
*/
void rknpu_host_release(void)
{
    if (!g_npu_ready)
        return;

    for (int i = 0; i < RKNPU_MAX_CORES; i++) {
        if (g_npu_base[i] == 0)
            continue;

        /* 用 gicv3_enable_int 而不是 irq_disable_irq：与上面 init 里的
           enable 成对, 且 gicv3.h 在本文件已经(间接)可见。
        */
        if (g_npu_irq[i] > 0)
            gicv3_enable_int(g_npu_irq[i], false);

        /* 给 guest 一个干净的起点：状态位全清, 不屏蔽任何位。
           INT_MASK 交给 guest 自己配(它会按任务描述符写掩码)。
        */
        npu_write(i, RKNPU_INT_CLEAR_ALL, RKNPU_INT_CLEAR);
        npu_write(i, 0, RKNPU_INT_MASK);
    }

    g_npu_ready = false;
    KLOG_INFO("rknpu: NPU handed over to guest (host IRQ 142/143/144 disabled, VMM polling injection enabled)\n");
}

/*
   rknpu_host_reclaim —— 从 guest 手里收回 NPU(vmm_arch_vm_destroy 时)。

   与 rknpu_host_release 严格逆序：先清状态位再开中断线。顺序不能反 ——
   guest 退出时 NPU 可能还挂着一个没被清的中断, 先开线的话宿主 ISR 会在
   enable 的那一刻立刻被拉起来(电平语义), 虽然只是白跑一次 ISR, 但
   "开线的瞬间执行了一次中断处理"对排障很不友好。先清掉就没有这个窗口。

   只回收当初确实初始化过的核(g_npu_cores>0)：QEMU 之类没有 NPU 的
   平台上 rknpu_init 早就 return 了, 这里必须是彻底的 no-op, 尤其不能把
   g_npu_ready 置成 true —— 那会让 rknpu_submit_task 以为硬件可用。

   幂等；与 release 配对使用。
*/
void rknpu_host_reclaim(void)
{
    if (g_npu_cores == 0 || g_npu_ready)
        return;

    for (int i = 0; i < RKNPU_MAX_CORES; i++) {
        if (g_npu_base[i] == 0)
            continue;

        npu_write(i, RKNPU_INT_CLEAR_ALL, RKNPU_INT_CLEAR);

        if (g_npu_irq[i] > 0)
            gicv3_enable_int(g_npu_irq[i], true);
    }

    g_npu_ready = true;
    KLOG_INFO("rknpu: NPU reclaimed from guest (host IRQs restored)\n");
}

/* ── 作业提交 ────────────────────────────────────────────────────────────── */

/*
   对一个核提交它自己的那一片任务。

   序列对齐 vendor `rknpu_job_subcore_commit()` + `_commit_pc()`(rknpu_job.c
   377/272), 也与 testos-reflector 的 job_commit_pc() 一致(真机跑过)。
   三处曾经"有意分歧"的地方现在都以 vendor 为准：

     1. 先写 PC_DATA_ADDR = 1 切 slave 模式。vendor 无条件写(只在
        pc_dma_ctrl 配置下包了个锁, 写的还是同一条)。
     2. 0x1004 / 0x3004 是 CNA/CORE 的 S_POINTER, 值里带核号：
        `0xe + 0x10000000 * core`。vendor 写的就是这个(参考项目只写 0xe,
        因为它的用例只跑 core 0, 看不出差别)。目标仍是该核自己的 base。
     3. PC_DMA_BASE_ADDR 写 0 —— librknnrt 把 args->task_base_addr 设成
        0, vendor 原样写下去, 等价。
*/
static void rknpu_job_commit(int core, void *task_base, uint32_t task_start, uint32_t task_number, uint32_t flags)
{
    rknpu_task_t *tasks = (rknpu_task_t *)task_base;
    uint32_t task_end = task_start + task_number - 1;
    rknpu_task_t *first = &tasks[task_start];
    rknpu_task_t *last = &tasks[task_end];

    uint32_t scale = rk3588_cfg.pc_data_amount_scale;
    uint32_t pp_en = (flags & RKNPU_JOB_PINGPONG) ? 1U : 0U;

    /* 切 slave 模式(本核) */
    npu_write(core, 0x1, RKNPU_PC_DATA_ADDR);

    /*
       切到本核的第 0 组寄存器。值里带核号：vendor 写的是
       `0xe + 0x10000000 * core_index`, 不是常量 0xe。

       core 0 时两者都是 0xe, 所以单核跑通完全看不出差别 —— 多核时写错
       就会让 core1/core2 的 CNA/CORE 指针指到错误的位置, 症状是"作业
       提交了但一个任务都不完成"。
    */
    uint32_t sp = RKNPU_S_POINTER_GROUP + 0x10000000U * (uint32_t)core;
    npu_write(core, sp, RKNPU_CNA_S_POINTER);
    npu_write(core, sp, RKNPU_CORE_S_POINTER);

    /* regcmd 地址 + 数据量(+EXTRA 再按 scale 向上取整) */
    npu_write(core, first->regcmd_addr, RKNPU_PC_DATA_ADDR);

    /*
       一次性把 regcmd 头几个字打出来 —— 用来和 guest 侧的同一份 dump 对比。

       为什么要这个：guest 里跑 NPU 时, 宿主侧已经验过"描述符地址在恒等窗口里,
       内容非零", 但作业仍然不完成；而 guest 的 librknnrt 会多打一条宿主上
       从没见过的 `REGTASK: The bit width of field value exceeds the limit`。
       两边跑的是同一个模型, 同一个 librknnrt —— 那么 regcmd 该逐字节相同。
       不同就说明问题在 librknnrt 生成 regcmd 的那条路上(模型/版本/环境),
       相同就说明问题在 NPU 的执行环境里。这一行日志就是那个判据。

       只打一次：提交路径在热路径上, 不能每次都刷。
    */
    if (!g_rknpu_regcmd_dumped) {
        const uint32_t *w = (const uint32_t *)(uintptr_t)phys_to_virt(first->regcmd_addr);

        g_rknpu_regcmd_dumped = true;
        KLOG_INFO("[rknpu] regcmd@0x%x first 8 words: %08x %08x %08x %08x %08x %08x %08x %08x\n", first->regcmd_addr,
                  w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);

        /*
           和 guest 侧同样的扫描(见 kernel/vmm/vdev/npu/vnpu.c)。

           判据是一次 A/B：guest 报出 regcmd[126] = 0x78015044 是个
           "窗口外地址", 但扫描是启发式的(任何落在 0x70000000..0x7FFFFFFF
           的值都会被拎出来), 一个常量也可能碰巧长这样。

             · 宿主这里 regcmd[126] 也是 0x78015044 → 它是数据常量,
               guest 那条报警是误报, 得换个方向查
             · 宿主这里是别的地址(宿主的 RAM 某处)→ 它是真的缓冲区地址,
               guest 的报警成立 —— 那个缓冲没从预留池里出

           模型和 librknnrt 两边完全相同, 所以这个对比是干净的。
        */
        {
            int bad = 0;

            for (uint32_t i = 0; i < 2048U; i++) {
                uint32_t v = w[i];

                if (v < 0x70000000U || v > 0x7FFFFFFFU)
                    continue;
                if (bad < 4)
                    KLOG_INFO("[rknpu]   host regcmd[%u] = 0x%08x\n", i, v);
                bad++;
            }
            KLOG_INFO("[rknpu]   %d values in host regcmd look like 0x7xxxxxxx addresses\n", bad);
        }
    }
    uint32_t amount = (first->regcfg_amount + RKNPU_PC_DATA_EXTRA_AMOUNT + scale - 1) / scale - 1;
    npu_write(core, amount, RKNPU_PC_DATA_AMOUNT);

    /* 期望中断掩码 / 清中断 */
    npu_write(core, last->int_mask, RKNPU_INT_MASK);
    npu_write(core, first->int_mask, RKNPU_INT_CLEAR);

    /* 任务控制：命令 0x6 | pingpong, 任务数在低 N 位 */
    uint32_t want_ctrl = ((0x6U | pp_en) << rk3588_cfg.pc_task_number_bits) | task_number;
    npu_write(core, want_ctrl, RKNPU_PC_TASK_CONTROL);

    npu_write(core, 0, RKNPU_PC_DMA_BASE_ADDR);

    /* 提交：OP_EN 1→0 */
    npu_write(core, 0x1, RKNPU_PC_OP_EN);
    npu_write(core, 0x0, RKNPU_PC_OP_EN);

    /*
       ── 这里曾经有一段"写完立刻回读, 不一致就报错", 已删除。留个记录,
          免得以后有人又想加回来。

       想法是好的：把"寄存器没写进去"(时钟/电源问题)和"写进去了但作业
       不跑"(描述符问题)分开 —— 这两种情况的现场都是"超时 + task_status=0"。

       但它在这个硬件上根本不成立：PC 侧的 DATA_ADDR / TASK_CONTROL
       是硬件消费型寄存器, PC 取走之后值就变了, 而我们的回读是跟它抢时间。
       真机上的两个反例正好互补, 而且两次推理结果都完全正确：

         第一次提交：DATA_ADDR 写 0x5b4abc0 回读 0x0, TASK_CONTROL 正常
         第二次提交：TASK_CONTROL 写 0xa 回读 0x0, DATA_ADDR 正常

       也就是说这个"不一致"是竞态, 不是故障 —— 每次跑的命中位置都不一样。
       代价还很实在：一次误报 5 行日志, 在 1.5 Mbaud 的 UART 上约 3.8ms,
       而它落在应用侧 `start.elapsed()` 的计时窗口里。实测同一份内核连跑两次
       得到 2.57ms / 6.40ms, 差值就是这段日志。

       真要"写没进去"的诊断, 看超时路径：那里已经逐核 dump 了完整的
       PC 寄存器快照 —— 作业没跑完才需要现场, 跑完了就不需要。
    */
}

/*
   ── 排障：超时时把「我们到底给硬件喂了什么」全打出来 ──────────────────

   为什么需要：任务描述符是用户态(librknnrt)填的, 我们只读不写。
   以前只在 QEMU 上跑过 DRM 层 —— 那里 npu.base0 为 0, rknpu_submit_task
   直接返回 -ENODEV, 这条数据通路从来没真正执行过。所以真机上第一次
   超时时, 最值得怀疑的就是描述符本身：regcmd_addr 是不是设备地址？
   regcmd 内容是不是对的？这片内存我们和用户态看到的是不是同一份？

   只在失败路径调用, 正常跑完不产生任何输出。
*/
static void rknpu_dump_task(const char *tag, const rknpu_task_t *t)
{
    KLOG_ERROR("[rknpu]   %s flags=0x%x op_idx=%u enable_mask=0x%x "
               "int_mask=0x%x int_clear=0x%x int_status=0x%x\n",
               tag, t->flags, t->op_idx, t->enable_mask, t->int_mask, t->int_clear, t->int_status);
    KLOG_ERROR("[rknpu]   %s regcfg_amount=%u regcfg_offset=0x%x "
               "regcmd_addr=0x%llx\n",
               tag, t->regcfg_amount, t->regcfg_offset, (unsigned long long)t->regcmd_addr);
}

static void rknpu_dump_regcmd(const rknpu_task_t *t)
{
    uint64_t off = 0;
    dma_buf_t *b = dma_buf_find_by_phys(t->regcmd_addr, &off);

    if (t->regcmd_addr == 0) {
        KLOG_ERROR("[rknpu]   regcmd_addr == 0 - user space did not fill it; NPU will fetch from address 0 (bus error "
                   "-> job never completes)\n");
        return;
    }
    if (!b) {
        KLOG_ERROR("[rknpu]   regcmd_addr is not in any dma-buf (not a buffer we exported?)\n");
        return;
    }

    uint8_t *base = (uint8_t *)dma_buf_kva(b);
    size_t bytes = dma_buf_size(b);
    /* 按 64 位字看 —— regcmd 就是 u64 数组(见参考项目 npu_regs) */
    if (off + 8U > bytes) {
        KLOG_ERROR("[rknpu]   regcmd_addr is past the end of the buffer (off=0x%llx)\n", (unsigned long long)off);
        return;
    }

    const uint64_t *w = (const uint64_t *)(base + off);
    size_t n = (bytes - off) / sizeof(uint64_t);
    if (n > 8U)
        n = 8U;

    KLOG_ERROR("[rknpu]   regcmd@buf+0x%llx first %zu words:", (unsigned long long)off, n);
    for (size_t i = 0; i < n; i++)
        kprintf(" %llx", (unsigned long long)w[i]);
    kprintf("\n");
}

/* 超时时把某个核的 PC 侧寄存器整片读回来 —— 一眼看出"跑到哪, 卡在哪" */
static void rknpu_dump_regs(int core)
{
    KLOG_ERROR("[rknpu]   core%d PC: OP_EN=0x%x DATA_ADDR=0x%x DATA_AMOUNT=0x%x "
               "INT_MASK=0x%x\n",
               core, npu_read(core, RKNPU_PC_OP_EN), npu_read(core, RKNPU_PC_DATA_ADDR),
               npu_read(core, RKNPU_PC_DATA_AMOUNT), npu_read(core, RKNPU_INT_MASK));
    KLOG_ERROR("[rknpu]   core%d PC: TASK_CONTROL=0x%x DMA_BASE=0x%x "
               "TASK_STATUS=0x%x INT_STATUS=0x%x\n",
               core, npu_read(core, RKNPU_PC_TASK_CONTROL), npu_read(core, RKNPU_PC_DMA_BASE_ADDR),
               npu_read(core, RKNPU_PC_TASK_STATUS), npu_read(core, RKNPU_INT_STATUS));
}

/*
   core_mask → 要驱动的核。

   vendor(rknpu_job_commit)只认这几种组合, 其余一律 "Unknown core mask"
   并且一个核都不提交：
       0x1 / 0x2 / 0x4 / 0x3(core0+1) / 0x7(三核)
   注意 0x5, 0x6 是不支持的 —— 别自作主张替它兜底, 照实拒绝,
   否则行为会偏离 librknnrt 的预期。

   AUTO(0) 走 vendor 的 rknpu_schedule_core_index()：挑累计任务数最少
   的核, 并强制单核。我们目前是同步提交(一路忙等到完成才返回), 设备永远
   处于空闲, 所以"负载最小的核"恒为 core 0 —— 这里如实反映这一点, 不假装
   做了负载均衡。将来改成异步(队列 + 每核 todo list)时, 这里要接上真的
   每核 task_num 记账。
*/
static int rknpu_pick_cores(uint32_t core_mask, int *cores, int *ncores)
{
    uint32_t mask = core_mask;

    if (mask == RKNPU_CORE_AUTO_MASK) {
        *ncores = 1;
        cores[0] = 0;
        return 0;
    }

    switch (mask) {
    case RKNPU_CORE0_MASK:
    case RKNPU_CORE1_MASK:
    case RKNPU_CORE2_MASK:
    case RKNPU_CORE0_MASK | RKNPU_CORE1_MASK:
    case RKNPU_CORE0_MASK | RKNPU_CORE1_MASK | RKNPU_CORE2_MASK:
        break;
    default:
        KLOG_ERROR("[rknpu] unsupported core_mask=0x%x (vendor rejects it too)\n", core_mask);
        return -22;
    }

    int n = 0;
    for (int i = 0; i < RKNPU_MAX_CORES; i++) {
        if (mask & (1U << i))
            cores[n++] = i;
    }

    /* 要的核必须在 platform.conf 里配了基址, 否则这个提交没法完整执行 */
    for (int i = 0; i < n; i++) {
        if (g_npu_base[cores[i]] == 0) {
            KLOG_ERROR("[rknpu] core_mask=0x%x requests core%d, but it has no MMIO base\n", core_mask, cores[i]);
            return -22;
        }
    }

    *ncores = n;
    return 0;
}

int rknpu_submit_task(void *task_base, size_t task_bytes, const struct rknpu_submit *s)
{
    if (!g_npu_ready || g_npu_cores == 0)
        return -19; /* ENODEV */
    if (!task_base || !s)
        return -22; /* EINVAL */
    if (s->task_number == 0)
        return -22;

    /*
       ── 任务按核切片。这一段是照 vendor 的 rknpu_job_subcore_commit_pc()
          (rknpu_job.c:307-330)补的, 之前漏了 —— 正是真机上"跑了 44 个就
          卡住"的根因。

       vendor 在 num_irqs > 1 的平台上(rk3588 是 3 个 IRQ)永远用
       subcore_task[] 覆盖 args->task_start/task_number：

           switch (job->use_core_num) {
           case 1: case 2: slice = subcore_task[core_index];
           case 3:         slice = subcore_task[core_index + 2];
           }

       也就是说它从不把 args 里的完整任务范围提交给某一个核。librknnrt
       把模型的任务数组按核切成若干片填进 subcore_task[], 每个核只跑自己那片。
       索引规则必须照抄：3 核时各片填在 [2..4], 单核/双核填在 [0..1]。
    */
    int cores[RKNPU_MAX_CORES];
    int ncores = 0;
    if (rknpu_pick_cores(s->core_mask, cores, &ncores) != 0)
        return -22;

    /* vendor 用请求的 core_mask 算 use_core_num, 切片索引也跟着它走
       —— 所以这个数在挑核之后按原始 core_mask 重新算一遍, 不能用 ncores
       顶替(AUTO 时 vendor 已经把 mask 改写成单核, 两者才一致)。
    */
    uint32_t use_core_num = (s->core_mask == RKNPU_CORE_AUTO_MASK) ? 1U : (uint32_t)ncores;

    size_t max_tasks = task_bytes / sizeof(rknpu_task_t);

    uint32_t start[RKNPU_MAX_CORES];
    uint32_t number[RKNPU_MAX_CORES];

    /*
       注意： 不要拿 task_start/task_number 去校验切片范围 —— 这是我之前
          自己发明的规则, 真机上证明是错的。

       一度以为 `subcore_task[i]` 是"job 范围内的一个子区间", 于是写了
       "切片必须落在 [task_start, task_start+task_number)" 的校验。真机上
       直接把正确的切片判成了非法, 退回单核完整范围, 然后作业跑一半卡死：

           job 范围 = [676, 688)
           subcore_task[0..4] = 672+4, 923+4, 672+4, 923+4, 998+4

       注意 4+4+4 = 12 正好等于该 job 的 task_number —— 每个核的任务片被
       放在任务数组里各自的区域, 并不落在 task_start 起的那个区间内。
       vendor 对此不做任何校验(rknpu_job_subcore_commit_pc 直接取值),
       照抄它就对了。

       唯一保留的是缓冲区边界检查：切片描述符是要真解引用的, 用户态给个
       start=1000000 不能变成内核读越界。这一条是内核自己的安全底线, 不是
       对 librknnrt 语义的猜测。不合法就直接失败而不是硬凑 —— 硬凑出来
       的作业会把 NPU 卡在半路, 比当场报错难查得多。
    */
    for (int i = 0; i < ncores; i++) {
        uint32_t core = (uint32_t)cores[i];
        uint32_t slice = (use_core_num == 3) ? core + 2U : core;

        start[i] = s->subcore_task[slice].task_start;
        number[i] = s->subcore_task[slice].task_number;

        if (number[i] == 0U || (size_t)start[i] + number[i] > max_tasks) {
            KLOG_ERROR("[rknpu] subcore_task[%u] out of range (start=%u number=%u max=%zu core_mask=0x%x) - submit "
                       "rejected\n",
                       slice, start[i], number[i], max_tasks, s->core_mask);
            return -22;
        }
    }

    /*
       每个核的期望中断掩码取自它自己那一片的最后一个任务
       (vendor：`job->int_mask[core_index] = last_task->int_mask`)。
       三片可以各不相同 —— 共用一个 expect 会让先完成的核把后完成的判成
       "状态不匹配"。
    */
    rknpu_task_t *tasks = (rknpu_task_t *)task_base;

    uint32_t expect[RKNPU_MAX_CORES];
    for (int i = 0; i < ncores; i++)
        expect[i] = tasks[start[i] + number[i] - 1].int_mask;

    /*
       注意： 这一行落在被计时的窗口里 —— 应用侧的 `start.elapsed()` 把整个
       SUBMIT 系统调用都算进去, 而 UART 是最贵的一环：实测板子跑 1.5 Mbaud,
       每字符约 6.7µs。真机上曾经因为多打约 770 字符让 MobileNetV1 从 2.57ms
       涨到 7.10ms —— 日志本身就是 4~5ms, 比推理还慢。

       所以这里只在多核时出声：单核(AUTO, 实测 librknnrt 给的就是 0x0)
       是常态, 没什么可看的；多核是新路径, 每次提交都值得一行。
    */
    if (ncores > 1) {
        KLOG_INFO("[rknpu] multi-core submit core_mask=0x%x -> %d core(s):", s->core_mask, ncores);
        for (int i = 0; i < ncores; i++)
            kprintf(" c%d[%u+%u]", cores[i], start[i], number[i]);
        kprintf(" use_core_num=%u\n", use_core_num);
        /*
           原始值一并打出来。切片的语义只能从 librknnrt 实际填的东西
           反推, 别靠猜 —— 第一次就是猜错了方向(以为切片必定落在 job
           范围内), 白跑一趟真机还顺带把 core0 卡住了。
        */
        KLOG_MODULE_DEBUG(LOG_MODULE_DRIVER,
                          "[rknpu]   job=[%u,%u) subcore_task: [0]=%u+%u [1]=%u+%u "
                          "[2]=%u+%u [3]=%u+%u [4]=%u+%u\n",
                          s->task_start, s->task_start + s->task_number, s->subcore_task[0].task_start,
                          s->subcore_task[0].task_number, s->subcore_task[1].task_start, s->subcore_task[1].task_number,
                          s->subcore_task[2].task_start, s->subcore_task[2].task_number, s->subcore_task[3].task_start,
                          s->subcore_task[3].task_number, s->subcore_task[4].task_start,
                          s->subcore_task[4].task_number);
    }
    KLOG_MODULE_DEBUG(
        LOG_MODULE_DRIVER,
        "[rknpu] submit flags=0x%x core_mask=0x%x -> %d core(s), use_core_num=%u, core0 tasks[%u..%u] (%u "
        "entries) expect=0x%x\n",
        s->flags, s->core_mask, ncores, use_core_num, cores[0], start[0], start[0] + number[0] - 1, number[0],
        expect[0]);

    uint32_t tmo_ms = s->timeout ? s->timeout : 1000;

    /*
       写 expect / 清状态 / 启动所有核, 必须在同一把锁里完成。
       分开做的话, NPU 快到中断落在"expect 还没写"的窗口里, 就会被判成
       状态不匹配 —— 那是个只有快机器上才偶发的假故障。

       多核时更要注意：先把所有核的 expect/state 都写好, 再统一放行。
       边写边提交的话, 先提交的核可能在最后一颗核的 expect 还没落笔时就
       打中断回来。
    */
    spin_lock(&g_job_lock);
    for (int i = 0; i < ncores; i++) {
        g_core_expect[cores[i]] = expect[i];
        g_core_state[cores[i]] = 0;
        /* 归零, 让"多核完成"那行报的是本次作业的数字而不是上次的 ——
           累计值没法证明哪个核刚刚干过活。
        */
        g_irq_count[cores[i]] = 0;
        g_core_status[cores[i]] = 0;
    }
    for (int i = 0; i < ncores; i++)
        rknpu_job_commit(cores[i], task_base, start[i], number[i], s->flags);
    spin_unlock(&g_job_lock);

    /*
       等所有核完成。vendor 也是这个语义 —— 它的 job 在网络里挂着一个
       interrupt_count(初值 = use_core_num), 归零才 wake_up 提交方。

       1us 步进紧密轮询(作业在微秒级完成, 睡一觉反而更慢), 上限 tmo_ms 毫秒。
    */
    uint32_t budget = tmo_ms * 1000U;
    bool all_done = false;
    while (budget-- > 0U) {
        all_done = true;
        for (int i = 0; i < ncores; i++) {
            int st = g_core_state[cores[i]];
            if (st < 0)
                return -5; /* EIO：中断状态不匹配, ISR 已打详情 */
            if (st == 0)
                all_done = false;
        }
        if (all_done) {
            /*
               ── 多核取证：证明三个核真的都在工作, 而不是我们只写了
                  寄存器, 硬件其实只跑了一个。

               两列数字都是从每个核自己的寄存器/中断线读来的, 不是我们
               写下去的回显 —— 这才是证据：

                 irq  : 该核独立中断线(142/143/144)上的中断次数, 由
                        各自的 ISR 累加。core1/core2 的 IRQ 响了, 就说明它们
                        各自跑完了自己的活。
                 done : 该核 PC_TASK_STATUS 低 12 位 = 它自己完成的任务数,
                        由硬件递增。

               单核时这两列没什么信息量, 所以只在多核时打 —— 这样常态不花
               UART 时间(见上面 KLOG_INFO 那段关于 6.7µs/字符的说明)。
            */
            if (ncores > 1) {
                KLOG_INFO("[rknpu] multi-core completion:");
                for (int i = 0; i < ncores; i++) {
                    int c = cores[i];
                    kprintf(" c%d[irq%d=%d status=0x%x]", c, g_npu_irq[c], g_irq_count[c], g_core_status[c]);
                }
                kprintf("\n");
            }
            return 0;
        }
        timer_delay_us(1);
    }

    KLOG_ERROR("[rknpu] job timeout (%u ms), across %d core(s):\n", tmo_ms, ncores);
    for (int i = 0; i < ncores; i++) {
        int c = cores[i];
        uint32_t task_status = npu_read(c, RKNPU_PC_TASK_STATUS);
        KLOG_ERROR("[rknpu]   core%d state=%d task_status=0x%x completed=%u/%u"
                   "  expect=0x%x irq_count=%d\n",
                   c, g_core_state[c], task_status, task_status & 0xFFF, number[i], g_core_expect[c], g_irq_count[c]);
        KLOG_ERROR("[rknpu]   core%d int_status=0x%x raw=0x%x\n", c, npu_read(c, RKNPU_INT_STATUS),
                   npu_read(c, RKNPU_INT_RAW_STATUS));
        rknpu_dump_regs(c);
    }

    /* 描述符是用户态填的 —— 把首尾两个原样打出来, 这是最可疑的一环 */
    KLOG_ERROR("[rknpu]   task_base=%p bytes=%zu\n", task_base, task_bytes);
    KLOG_ERROR("[rknpu]   args: flags=0x%x core_mask=0x%x use_core_num=%u "
               "task_base_addr=0x%llx\n",
               s->flags, s->core_mask, use_core_num, (unsigned long long)s->task_base_addr);
    KLOG_ERROR("[rknpu]   subcore_task: [0]=%u+%u [1]=%u+%u [2]=%u+%u "
               "[3]=%u+%u [4]=%u+%u\n",
               s->subcore_task[0].task_start, s->subcore_task[0].task_number, s->subcore_task[1].task_start,
               s->subcore_task[1].task_number, s->subcore_task[2].task_start, s->subcore_task[2].task_number,
               s->subcore_task[3].task_start, s->subcore_task[3].task_number, s->subcore_task[4].task_start,
               s->subcore_task[4].task_number);

    for (int i = 0; i < ncores; i++) {
        int c = cores[i];
        uint32_t slice = (use_core_num == 3) ? (uint32_t)c + 2U : (uint32_t)c;
        KLOG_ERROR("[rknpu]   core%d subcore_task[%u] = %u+%u\n", c, slice, start[i], number[i]);
        rknpu_dump_task("first:", &tasks[start[i]]);
        rknpu_dump_task("last: ", &tasks[start[i] + number[i] - 1]);
        rknpu_dump_regcmd(&tasks[start[i]]);
    }

    /*
       抽看任务表的 op 分布(只对第一个核做, 免得三份日志互相比对)：
       librknnrt 是按 op 分段填这张表的, 段边界就是它切核的地方。切片取对了
       的话, 该核 range 的末尾应当正好落在某个 op 的最后一个任务上。
    */
    KLOG_ERROR("[rknpu]   core%d op distribution:\n", cores[0]);
    for (uint32_t i = 0; i < number[0]; i += (number[0] / 6U) + 1U) {
        rknpu_task_t *t = &tasks[start[0] + i];
        KLOG_ERROR("[rknpu]   task[%u] op_idx=%u enable_mask=0x%x "
                   "regcfg_amount=%u regcmd=0x%llx\n",
                   start[0] + i, t->op_idx, t->enable_mask, t->regcfg_amount, (unsigned long long)t->regcmd_addr);
    }

    return -5;
}
