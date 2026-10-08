/* kernel/vmm/vdev/npu/vnpu.c
 *
 * RK3588 NPU 寄存器透传 —— 见 include/vmm/vmm_vnpu.h 顶上那段说明。
 *
 * 本文件只做一件事：把 guest 的 MMIO 访问按宽度转发到真实寄存器。
 * 没有状态机、没有影子寄存器、不解释任何位 —— 这是**透传**，不是模拟。
 * 也正因为如此，guest 里的驱动行为与真机完全一致，出问题时不会多一层
 * "到底是 VMM 模拟错了还是驱动错了"的疑问。
 */

#include "vmm/vmm_vnpu.h"
#include "mmio.h"
#include "klog.h"
#include "platform_cfg.h"
#include "guest_loader.h" /* GUEST_LINUX_NPU_DMA_*：描述符地址越界检查 */
#include "mm_vm.h"         /* phys_to_virt：dump 恒等窗口里的 regcmd */
#include "types.h"
/*
 * ⚠️ 本文件对宿主 NPU 驱动的依赖是**编译期**的：寄存器偏移复用
 *    driver/npu/rknpu.h，让出/收回要调 rknpu_host_release/reclaim。
 *    而 rknpu.c 只在 NPU=rknpu 时才进构建（Makefile §6b），
 *    别的平台上直接链接失败——现象是
 *      `undefined reference to rknpu_host_release`
 *    指向这个 VMM 文件，而根因在构建配置里。
 *
 *    所以整段用 DRIVER_NPU_RKNPU 包起来：那种平台上本文件退化成
 *    「只转发、不注入」，而 vnpu_init 本来也会因为 host_base==0 提前返回
 *    （QEMU 的 platform.conf 里没有 npu 段）。两层都留着 —— 前者是编译期
 *    的硬边界，后者是运行期的。
 */
#if DRIVER_NPU_RKNPU
#include "npu/rknpu.h"
#include "npu/npu_mmu.h" /* 宿主独占的 NPU IOMMU */
#endif

/*
 * NPU 的 IOMMU 寄存器块 —— 落在**本透传窗口之内**，必须摘掉不能转发。
 *
 * vendor DTS 给的四个块（0xFDAB9000 / 0xFDABA000 / 0xFDACA000 / 0xFDADA000）
 * 换算到本窗口的偏移就是下面这四个。RK3588 的 MMU 是 Rockchip 自研的**单级**
 * IOMMU：一张表，谁写谁说了算 —— guest 要是够得着，它就能把任意物理地址
 * 映射进来，宿主好不容易拿到的隔离立刻归零。
 *
 * 窗口内偏移：核0+0x9000、核0+0xA000、核1+0xA000、核2+0xA000。
 */
static inline bool vnpu_is_mmu_reg(uint64_t off)
{
    uint64_t page = off & ~0xFFULL;

    return page == 0x9000ULL || page == 0xA000ULL || page == 0x1A000ULL ||
           page == 0x2A000ULL;
}

/* 宿主侧 NPU 寄存器基址（platform_get_mmio 的返回值，已含 KERNEL_VMA）。
 * 只在 vnpu_init 里写一次；= 0 表示本平台没有 NPU。 */
static uintptr_t g_vnpu_host_base = 0;

/*
 * 当前把 NPU 拿走的 VM。-1 = 没人在用（宿主自己拿着）。
 *
 * 为什么要有这个守卫：NPU 是**单实例**硬件，三核共享一组 PC 寄存器和
 * 电源域，两个 VM 同时透传必然互相踩（各自以为独占、各自写
 * PC_DATA_ADDR/PC_OP_EN），结果是谁都跑不对，且两边日志都看不出异常。
 * 硬件没有隔离手段（SMMU 我们没启），所以只能在 VMM 这一层拒绝第二个
 * 申请者 —— 拒绝是可诊断的，静默错乱不是。
 */
static bool g_vnpu_owned;
static uint32_t g_vnpu_owner_vmid;

/* 统计：guest 一共访问了多少次（诊断用，只在首次打印） */
static volatile uint64_t g_vnpu_accesses = 0;

/*
 * guest 写过的偏移，**每个偏移只记一次**（最多 8 个）。
 *
 * 为什么需要：判断"作业有没有真的提交给硬件"不能只看有没有访问 ——
 * guest 明明读到了 VERSION（说明转发通了），作业却 6 秒超时、task counter 为 0。
 * 那种情况下必须知道 guest 到底**写了哪几个寄存器**：写了 PC_OP_EN(0x08) 说明
 * 提交路径走通了、问题在其后的 DMA；只写了几个配置寄存器就没了，说明卡在前面。
 * 没有这个，两种情况的日志一模一样。
 *
 * 上限 24 条而不是全记：一次提交要写十几个寄存器，全记会把 UART 刷满。
 *
 * ⚠️ 原来是 8，**太小了** —— 真机上第一次跑就正好卡在 8：guest 依次写了
 *    0x10 / 0x1004 / 0x3004 / 0x14 / 0x20 / 0x24 / 0x30 / 0x34，
 *    而 vendor 的提交序列里 **PC_OP_EN(0x08) 是最后一个写的**，正好是第 9 个，
 *    被上限吃掉了。于是日志里看不出"作业到底提交了没有" —— 上限本身把
 *    它要回答的那个问题给遮住了。
 */
#define VNPU_TRACE_MAX 24
static uint32_t g_vnpu_wrote[VNPU_TRACE_MAX];
static int g_vnpu_nwrote;

/*
 * 提交路径上的关键寄存器 —— 这几个**每次写都记**，不做去重。
 *
 * ⚠️ 为什么必须区别对待：上面那个"每个偏移只记一次"对这几个恰好是错的，
 *    因为它们**本来就会被写多次**，而第一次写的值往往不是关键的那个：
 *
 *      PC_DATA_ADDR(0x10)  先写 1 切 slave 模式，**第二次才写真正的
 *                          描述符地址** —— 那正是判断"NPU 读不读得到 regcmd"
 *                          的唯一依据，被去重吃掉了
 *      PC_OP_EN(0x08)      写 1 再写 0（1→0 才是提交），只记第一次等于
 *                          只看到"开始提交"、看不到"提交完成"
 *      PC_DATA_AMOUNT(0x14) / PC_DMA_BASE_ADDR(0x34)  同理，多核时每核各写一遍
 *
 *    真机上连着两轮看到的都是 0x10=0x1 / 0x8=0x1，真正的地址一次都没露过面。
 */
#define VNPU_KEY_MAX 16
static int g_vnpu_nkey;
#if DRIVER_NPU_RKNPU
static int g_vnpu_dumped; /* regcmd 只 dump 一次（只用在上面的 #if 块里）*/
#endif

static inline bool vnpu_key_off(uint64_t off)
{
    return off == 0x08 || off == 0x10 || off == 0x14 || off == 0x34;
}

/* guest 碰 MMU 寄存器 —— 只报一次，然后静默忽略（不是致命错，但它意味着
 * guest 的 DTB 里写了 iommus，而我们是按"guest 没有 IOMMU"设计的）*/
static void vnpu_mmu_touched(const char *what, uint64_t off)
{
    static bool once;

    if (!once) {
        once = true;
        KLOG_ERROR("[vnpu] ⚠ guest %s NPU 的 MMU 寄存器 (offset=0x%llx) —— "
                   "已忽略。宿主要独占它，见 include/npu/npu_mmu.h\n",
                   what, (unsigned long long)off);
    }
}

static inline volatile void *vnpu_reg(uint64_t offset)
{
    return (volatile void *)(g_vnpu_host_base + offset);
}

static uint64_t vnpu_read(mmio_device_t *dev, uint64_t offset, uint8_t size)
{
    (void)dev;

    if (offset + size > VNPU_SIZE)
        return 0;

    if (vnpu_is_mmu_reg(offset)) {
        vnpu_mmu_touched("读", offset);
        return 0;
    }

    if (++g_vnpu_accesses == 1) {
        KLOG_INFO("[vnpu] guest 首次访问 NPU 寄存器 (offset=0x%llx size=%u) "
                  "—— 透传已生效\n",
                  (unsigned long long)offset, size);
    }

    /* 按访问宽度转发。NPU 寄存器是 32 位，但驱动可能用 8/16 位读同一片地址
     * （比如探测），照宽度回即可。 */
    switch (size) {
    case 1:
        return read8(vnpu_reg(offset));
    case 2:
        return read16(vnpu_reg(offset));
    case 4:
        return read32(vnpu_reg(offset));
    default:
        return 0;
    }
}

static void vnpu_write(mmio_device_t *dev, uint64_t offset, uint8_t size,
                       uint64_t value)
{
    (void)dev;

    if (offset + size > VNPU_SIZE)
        return;

    if (vnpu_is_mmu_reg(offset)) {
        vnpu_mmu_touched("写", offset);
        return;
    }

    /*
     * 提交点：guest 写 PC_OP_EN(0x08) = 1，NPU 马上要开始 DMA。
     * 在这之前把 stage-2 的映射镜像进 NPU 的 IOMMU 表 —— 这是"NPU 能看到的
     * 内存"的唯一权威来源，也是窗口外缓冲不再出错的原因。
     */
#if DRIVER_NPU_RKNPU
    if (offset == RKNPU_PC_OP_EN && (value & 1U) && dev->priv)
        (void)npu_mmu_sync((vm_t *)dev->priv);
#endif

    if (++g_vnpu_accesses == 1) {
        KLOG_INFO("[vnpu] guest 首次访问 NPU 寄存器 (offset=0x%llx size=%u) "
                  "—— 透传已生效\n",
                  (unsigned long long)offset, size);
    }

    /* 关键寄存器每次写都记（见 vnpu_key_off 的说明）*/
    if (vnpu_key_off(offset) && g_vnpu_nkey < VNPU_KEY_MAX) {
        g_vnpu_nkey++;
        KLOG_INFO("[vnpu] guest 写 NPU reg 0x%llx = 0x%llx\n",
                  (unsigned long long)offset, (unsigned long long)value);
    }

    /*
     * 描述符地址越界检查 —— 这是"NPU 读不读得到 regcmd"的直接判据。
     * 0x10 的值可能是 1（切 slave 的哨兵）或一个真地址；真地址必须落在
     * 恒等窗口里，否则 NPU 按物理地址去读会读到别处的东西，而**两边都不报错**：
     * guest 侧驱动只管写下去，NPU 那边只是跑不出结果。
     */
#if DRIVER_NPU_RKNPU
    if (offset == RKNPU_PC_DATA_ADDR && value > 1 &&
        GUEST_LINUX_NPU_DMA_SIZE != 0) {
        uint64_t base = GUEST_LINUX_NPU_DMA_GPA;
        uint64_t lim = base + GUEST_LINUX_NPU_DMA_SIZE;

        if (value < base || value >= lim) {
            KLOG_ERROR("[vnpu] ⚠ guest 把描述符地址写在 0x%llx，"
                       "不在 NPU 恒等窗口 [0x%llx, 0x%llx) 里 —— "
                       "NPU 读到的不是这份 regcmd，作业必然跑不出来\n",
                       (unsigned long long)value, (unsigned long long)base,
                       (unsigned long long)lim);
        } else if (!g_vnpu_dumped) {
            /*
             * 地址在窗口里 —— 但那只说明**这份 regcmd** 放对了地方。
             *
             * regcmd 里还带着权重/输入/输出缓冲的物理地址（它们是寄存器写的
             * **值**，不是 PC 侧那几条描述符），NPU 会照着这些地址去读写。
             * 只要其中任何一个落在窗口外，NPU 就会去访问**宿主的物理内存** ——
             * 那块地址在宿主眼里根本不是 guest 的页，于是：
             *   · NPU 的输出写进了别人的内存 → guest 读到的输出永远是垃圾
             *   · 写坏的那块如果是别的 guest 页 → guest 内核莫名 Oops
             * 而**两边都不报错**：guest 只管提交，NPU 只管写。
             *
             * 所以这里把整份 regcmd 扫一遍，把"像物理地址、又不在窗口里"的
             * 值全部列出来。一次性、有上限，不会刷屏。
             */
            const uint32_t *w = (const uint32_t *)(uintptr_t)
                phys_to_virt((uint64_t)value);
            uint32_t nwords = 0x226U * 2U; /* PC_DATA_AMOUNT 个 8 字节单元 */
            uint64_t base = GUEST_LINUX_NPU_DMA_GPA;
            uint64_t lim = base + GUEST_LINUX_NPU_DMA_SIZE;
            int bad = 0;

            g_vnpu_dumped = 1;
            if (nwords > 2048U)
                nwords = 2048U; /* 别在热路径上扫太多 */

            KLOG_INFO("[vnpu] regcmd@0x%llx 头 8 字: %08x %08x %08x %08x "
                      "%08x %08x %08x %08x (扫 %u 字)\n",
                      (unsigned long long)value, w[0], w[1], w[2], w[3],
                      w[4], w[5], w[6], w[7], nwords);

            for (uint32_t i = 0; i < nwords; i++) {
                uint32_t v = w[i];

                /* 只挑"看着像这段 guest RAM 的物理地址"的值：
                 * 0x70000000..0x7FFFFFFF。窗口是它的一个子区间。*/
                if (v < 0x70000000U || v > 0x7FFFFFFFU)
                    continue;
                if ((uint64_t)v >= base && (uint64_t)v < lim)
                    continue;
                if (bad < 8)
                    KLOG_ERROR("[vnpu] ⚠ regcmd[%u] = 0x%08x 是越界地址 —— "
                               "不在 NPU 窗口 [0x%llx, 0x%llx)\n",
                               i, v, (unsigned long long)base,
                               (unsigned long long)lim);
                bad++;
            }
            if (bad)
                KLOG_ERROR("[vnpu] 共 %d 个字是窗口外的地址 —— NPU 会去写"
                           "宿主的物理内存，输出必然是垃圾\n", bad);
            else
                KLOG_INFO("[vnpu] regcmd 里的地址全在窗口内 ✓\n");
        }
    }
#endif

    /* 其余偏移每个只记一次（见 g_vnpu_wrote 的说明）*/
    if (g_vnpu_nwrote < VNPU_TRACE_MAX) {
        int i, seen = 0;

        for (i = 0; i < g_vnpu_nwrote; i++) {
            if (g_vnpu_wrote[i] == (uint32_t)offset) {
                seen = 1;
                break;
            }
        }
        if (!seen) {
            g_vnpu_wrote[g_vnpu_nwrote++] = (uint32_t)offset;
            KLOG_INFO("[vnpu] guest 写 NPU reg 0x%llx = 0x%llx (size=%u)\n",
                      (unsigned long long)offset, (unsigned long long)value,
                      size);
        }
    }

    switch (size) {
    case 1:
        write8((uint8_t)value, vnpu_reg(offset));
        break;
    case 2:
        write16((uint16_t)value, vnpu_reg(offset));
        break;
    case 4:
        write32((uint32_t)value, vnpu_reg(offset));
        break;
    default:
        break;
    }
}

static const mmio_dev_ops_t g_vnpu_ops = {
    .name = "vnpu",
    .base = VNPU_BASE,
    .size = VNPU_SIZE,
    .read = vnpu_read,
    .write = vnpu_write,
};

int vnpu_init(vm_t *vm, mmio_bus_t *bus, uintptr_t host_base)
{
    /*
     * 平台没有 NPU（QEMU、其它板子）→ 不注册设备。
     * 这不是错误：guest 的 DTS 里也不会有 npu 节点。
     */
    if (host_base == 0) {
        KLOG_INFO("[vnpu] 本平台无 NPU，跳过透传设备注册\n");
        return 0;
    }

    if (g_vnpu_owned && g_vnpu_owner_vmid != vm->vmid) {
        KLOG_ERROR("[vnpu] NPU 已被 vm%d 占用，拒绝 vm%d 的申请 —— "
                   "单实例硬件，不支持两个 VM 同时透传\n",
                   g_vnpu_owner_vmid, vm->vmid);
        return -1;
    }

    /*
     * ⚠️ ops/priv 必须在 mmio_bus_register **之前**填好。
     *
     * mmio_bus_register() 第一件事就是读 `dev->ops->name / ->base / ->size`
     * 打那行 "[mmio] registered '...'" —— 此时 ops 还是 NULL（vm_t 整个被
     * memset 过），于是读 0 号地址：
     *     [el1_sync] EC=0x25 ESR=0x96000007 FAR=0x0
     *     #0 mmio_bus_register+0x6c
     *     #1 vnpu_init+0x50
     * 真机上第一次跑到就崩在这儿。vpl011_init 是按这个顺序写的
     * （先 dev.ops = ... 再 register），照它来。
     */
    vm->vnpu_dev.ops = &g_vnpu_ops;
    /* sync 要查这个 VM 的 stage-2，而 vnpu_write 只拿得到 dev */
    vm->vnpu_dev.priv = vm;
    g_vnpu_host_base = host_base;

    if (mmio_bus_register(bus, &vm->vnpu_dev) != 0) {
        KLOG_ERROR("[vnpu] MMIO 总线注册失败（设备数超过 %d）\n",
                   MMIO_MAX_DEVICES);
        return -1;
    }

    /*
     * 让宿主驱动放手：关掉它在 GIC 里使能的 142/143/144。
     * 不做的后果见 rknpu_host_release() 的注释（宿主 ISR 会抢先把 guest
     * 的中断清掉，而且两边都不报错）。
     *
     * 放在**注册成功之后**：万一上面失败、建 VM 半途而废，宿主手里还拿着
     * NPU，不会出现"谁都没有 NPU"的空窗。
     */
#if DRIVER_NPU_RKNPU
    rknpu_host_release();
    /*
     * 把 NPU 的地址翻译接管过来：guest 交给 NPU 的是 IPA，由宿主的表
     * 翻到真实物理页 —— 这样它给什么缓冲都行，不再受恒等窗口限制。
     */
    if (npu_mmu_init() == 0)
        (void)npu_mmu_enable(vm->cfg.mem_base, vm->cfg.mem_size);
#endif
    g_vnpu_owned = true;
    g_vnpu_owner_vmid = vm->vmid;

    KLOG_INFO("[vnpu] NPU 透传已注册: guest 0x%llx..0x%llx -> host 0x%lx\n",
              (unsigned long long)VNPU_BASE,
              (unsigned long long)(VNPU_BASE + VNPU_SIZE),
              (unsigned long)host_base);
    return 0;
}

void vnpu_destroy(vm_t *vm)
{
    if (!g_vnpu_owned || g_vnpu_owner_vmid != vm->vmid)
        return;

    g_vnpu_owned = false;
    /* 交还给宿主 —— 这样停掉 VM 之后还能再起一个（vmm_helper_regress.sh
     * 的 --restart 正是走这条路），宿主自己也能接着跑 NPU 作业。*/
#if DRIVER_NPU_RKNPU
    /* ⚠️ 关翻译在前：宿主 reclaim 之后自己驱动 NPU 用的是物理地址 */
    npu_mmu_disable();
    rknpu_host_reclaim();
#endif
}

/*
 * 中断线是否有效 —— 判据是 `(INT_STATUS & INT_MASK) != 0`。
 *
 * ⚠️⚠️ **别写成 `INT_STATUS & ~INT_MASK`** —— 那是把掩码反着用了，
 *      结果**恒为 0**，于是一次都不会注入。真机上就是这么错的：
 *      NPU 从 `no-map` 修好之后**一直在正常完成作业**
 *      （guest 里 `INT_STATUS=0x100`，宿主里同一份作业也是 `0x100`，
 *      而宿主的驱动判它成功、打出了检测框），
 *      但 guest 的 ISR 永远不跑 → 6 秒超时、task counter 为 0。
 *      所有寄存器回读、regcmd、DMA 全都正常，**症状完全指向 NPU 没跑**。
 *
 *      为什么 `& INT_MASK` 才对：实测 `INT_STATUS == INT_RAW_STATUS & INT_MASK`
 *      （raw=0xc0000151, mask=0x300, status=0x100 —— 逐位吻合），也就是
 *      INT_STATUS **本身就已经是掩码后的值**，再 `& ~mask` 就抹干净了。
 *      写成 `& INT_MASK` 对两种解释都对：INT_STATUS 已经是掩码值时它是恒等，
 *      万一它是原始值也照样正确。`& ~mask` 则只在"INT_STATUS 是原始值"这种
 *      本硬件并不成立的解释下才对。
 *
 * 三核各自独立判：一次作业可能只跑一个核（AUTO/单核掩码），也可能三核
 * 一起完成但**先后差几微秒**，所以不能"任一核有效就投三条线"，只能各投各的。
 *
 * 这里只读两个寄存器、不写任何东西，见头文件里"只读，不 ack"那条。
 */
bool vnpu_irq_asserted(vm_t *vm, uint32_t core)
{
    (void)vm; /* 目前三核全局唯一；留参数是为了将来能按 VM 区分 */

#if !DRIVER_NPU_RKNPU
    /* 本构建里没有宿主 NPU 驱动（NPU≠rknpu）—— 连寄存器偏移都没有，
     * 也就无从谈透传。见文件顶上那段。*/
    (void)core;
    return false;
#else
    uintptr_t base;
    uint32_t status, mask;

    if (!g_vnpu_owned || g_vnpu_host_base == 0 || core >= VNPU_CORES)
        return false;

    base = g_vnpu_host_base + (uintptr_t)core * VNPU_CORE_SIZE;
    status = read32((volatile void *)(base + RKNPU_INT_STATUS));
    mask = read32((volatile void *)(base + RKNPU_INT_MASK));

    return (status & mask) != 0;
#endif
}
