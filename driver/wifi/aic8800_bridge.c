#include "wifi/aic8800_bridge.h"

#include "arch.h"
#include "irq/plic.h"
#include "klog.h"
#include "mm_vm.h"
#include "mmio.h"
#include "task/task.h"
#include "timer/timer.h"
#include "wifi_api.h"

#ifndef DEVICE_SDIO1_BASE_RAW
#define DEVICE_SDIO1_BASE_RAW 0UL
#endif
#ifndef DEVICE_SDIO1_IRQ
#define DEVICE_SDIO1_IRQ 0U
#endif
#ifndef DEVICE_SDIO1_CRG_RAW
#define DEVICE_SDIO1_CRG_RAW 0UL
#endif
#ifndef DEVICE_SDIO1_SYSCTRL_RAW
#define DEVICE_SDIO1_SYSCTRL_RAW 0UL
#endif
#ifndef DEVICE_SDIO1_RTCSYS_CTRL_RAW
#define DEVICE_SDIO1_RTCSYS_CTRL_RAW 0UL
#endif
#ifndef DEVICE_SDIO1_RTCSYS_IO_RAW
#define DEVICE_SDIO1_RTCSYS_IO_RAW 0UL
#endif
#ifndef DEVICE_WIFI_GPIOE_RAW
#define DEVICE_WIFI_GPIOE_RAW 0UL
#endif
#ifndef DEVICE_WIFI_POWERON_PIN
#define DEVICE_WIFI_POWERON_PIN 0U
#endif
#ifndef DEVICE_WIFI_WAKEUP_PIN
#define DEVICE_WIFI_WAKEUP_PIN 0U
#endif

#define DW_GPIO_SWPORTA_DR  0x000U
#define DW_GPIO_SWPORTA_DDR 0x004U

int wifi_task_spawn(const char *name, void (*entry)(void *), void *arg, uint8_t priority)
{
    task_t *task = task_create(name, entry, arg, priority);
    if (!task) {
        KLOG_ERROR("[wifi] failed to create worker task '%s'\n", name ? name : "<null>");
        return -1;
    }

    KLOG_INFO("[wifi] worker task '%s' created: id=%u\n", task->name, task->id);
    return 0;
}

static uintptr_t wifi_mmio(uintptr_t raw)
{
#if DEVICE_MMIO_NEEDS_VMA
    return raw ? raw + KERNEL_VMA : 0;
#else
    return raw;
#endif
}

static void wifi_gpio_set_high(uintptr_t gpio_base, unsigned pin)
{
    uint32_t mask = 1U << pin;
    uint32_t ddr = read32((const volatile void *)(gpio_base + DW_GPIO_SWPORTA_DDR));
    write32(ddr | mask, (volatile void *)(gpio_base + DW_GPIO_SWPORTA_DDR));

    uint32_t dr = read32((const volatile void *)(gpio_base + DW_GPIO_SWPORTA_DR));
    write32(dr | mask, (volatile void *)(gpio_base + DW_GPIO_SWPORTA_DR));
}

static void wifi_gpio_set_low(uintptr_t gpio_base, unsigned pin)
{
    uint32_t mask = 1U << pin;
    uint32_t ddr = read32((const volatile void *)(gpio_base + DW_GPIO_SWPORTA_DDR));
    write32(ddr | mask, (volatile void *)(gpio_base + DW_GPIO_SWPORTA_DDR));

    uint32_t dr = read32((const volatile void *)(gpio_base + DW_GPIO_SWPORTA_DR));
    write32(dr & ~mask, (volatile void *)(gpio_base + DW_GPIO_SWPORTA_DR));
}

static void wifi_power_up_from_dts(void)
{
    uintptr_t gpioe = wifi_mmio(DEVICE_WIFI_GPIOE_RAW);
    if (!gpioe) {
        KLOG_WARN("[wifi] wifi_pin GPIOE not configured; skip board power pins\n");
        return;
    }

    wifi_gpio_set_low(gpioe, DEVICE_WIFI_WAKEUP_PIN);
    wifi_gpio_set_low(gpioe, DEVICE_WIFI_POWERON_PIN);
    timer_spin(4000U * 50U);

    wifi_gpio_set_high(gpioe, DEVICE_WIFI_POWERON_PIN);
    timer_spin(4000U * 200U);
    wifi_gpio_set_high(gpioe, DEVICE_WIFI_WAKEUP_PIN);

    uint32_t ddr = read32((const volatile void *)(gpioe + DW_GPIO_SWPORTA_DDR));
    uint32_t dr = read32((const volatile void *)(gpioe + DW_GPIO_SWPORTA_DR));
    KLOG_INFO("[wifi] wifi_pin applied: gpioe=0x%llx poweron=PE%u wakeup=PE%u ddr=0x%08x dr=0x%08x\n",
              (unsigned long long)gpioe,
              (unsigned)DEVICE_WIFI_POWERON_PIN,
              (unsigned)DEVICE_WIFI_WAKEUP_PIN,
              ddr,
              dr);

    timer_spin(4000U * 500U);
}

static void wifi_sdio1_plic_handler(uint32_t irq, void *ctx)
{
    (void)irq;
    (void)ctx;
    wifi_sdio1_irq_handler();
}

static void wifi_register_sdio1_irq(void)
{
    if (DEVICE_SDIO1_IRQ == 0) {
        KLOG_WARN("[wifi] SDIO1 IRQ not configured; CARD_INT disabled\n");
        return;
    }

#if ARCH_RISCV64
    plic_install(DEVICE_SDIO1_IRQ, wifi_sdio1_plic_handler, NULL);
    KLOG_INFO("[wifi] SDIO1 controller IRQ %u registered\n", (unsigned)DEVICE_SDIO1_IRQ);
#else
    KLOG_WARN("[wifi] SDIO1 IRQ registration is only implemented on RISC-V\n");
#endif
}

static void aic8800_wifi_init_task(void *arg)
{
    (void)arg;

    KLOG_INFO("[wifi] initializing AIC8800 runtime glue\n");
    int rc = wifi_runtime_init();
    if (rc != 0) {
        KLOG_ERROR("[wifi] runtime init failed rc=%d\n", rc);
        return;
    }
    KLOG_INFO("[wifi] AIC8800 runtime ready\n");

    wifi_power_up_from_dts();
    wifi_register_sdio1_irq();

    uintptr_t crg = wifi_mmio(DEVICE_SDIO1_CRG_RAW);
    uintptr_t sysctrl = wifi_mmio(DEVICE_SDIO1_SYSCTRL_RAW);
    uintptr_t rtcsys_ctrl = wifi_mmio(DEVICE_SDIO1_RTCSYS_CTRL_RAW);
    uintptr_t rtcsys_io = wifi_mmio(DEVICE_SDIO1_RTCSYS_IO_RAW);
    uintptr_t base = wifi_mmio(DEVICE_SDIO1_BASE_RAW);

    rc = wifi_sdio1_probe(crg, sysctrl, rtcsys_ctrl, rtcsys_io, base);
    if (rc != 0) {
        KLOG_ERROR("[wifi] SDIO1 probe failed rc=%d\n", rc);
        return;
    }

#if ARCH_RISCV64
    if (DEVICE_SDIO1_IRQ != 0)
        plic_enable_irq(DEVICE_SDIO1_IRQ, 1);
#endif

    KLOG_INFO("[wifi] AIC8800 probe completed\n");
}

void aic8800_wifi_start_from_platform(void)
{
    task_t *task = task_create("wifi-init", aic8800_wifi_init_task, NULL, 20);
    if (!task) {
        KLOG_ERROR("[wifi] failed to create wifi-init task\n");
        return;
    }

    KLOG_INFO("[wifi] wifi-init task created: id=%u\n", task->id);
}
