#!/usr/bin/env python3
"""
gen_platform.py  —  从 platform.conf 静态表生成 platform.mk 和 platform_static.c

用法:
  python3 tools/gen_platform.py <platform.conf> <build/platform.mk> <include/>
"""

import sys
import os
import re


def write_if_changed(path, content, mode='w'):
    try:
        with open(path, 'r' if 'b' not in mode else 'rb') as f:
            if f.read() == content:
                return
    except FileNotFoundError:
        pass
    with open(path, mode) as f:
        f.write(content)


# ─── 解析嵌套 platform.conf ──────────────────────────────────────────────────

def _extract_block(text, key):
    """找到 key = { ... }（平衡括号）并返回大括号内的内容字符串。"""
    m = re.search(rf'\b{re.escape(key)}\s*=\s*\{{', text)
    if not m:
        return ''
    start = m.end()
    depth = 1
    pos = start
    while pos < len(text) and depth > 0:
        c = text[pos]
        if c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
        pos += 1
    return text[start:pos - 1] if depth == 0 else ''


def _find_str(text, key):
    m = re.search(rf'\b{re.escape(key)}\s*=\s*"([^"]*)"', text)
    return m.group(1) if m else ''


def _find_num(text, key, default='0'):
    m = re.search(rf'\b{re.escape(key)}\s*=\s*(0x[0-9a-fA-F]+|[0-9]+)', text)
    return m.group(1) if m else default


def _find_bool(text, key):
    m = re.search(rf'\b{re.escape(key)}\s*=\s*(true|false)', text)
    return '1' if (m and m.group(1) == 'true') else '0'


def parse_platform_conf(path):
    """
    解析嵌套 platform.conf 格式，返回与所有 write_* 函数兼容的 conf 字典。

    platform.conf 结构:
        local platform = {
            arch = "aarch64",
            name = "QEMU",
            memory  = { ram = {base,size}, rootfs = {base,size,mb}, reserves = {...} },
            mmio_vma = true/false,
            lapic    = true/false,
            uart  = { driver, base, reg_shift },
            irq   = { driver, gicd, gicc, gich, gicr, plic, clint },
            timer = { driver, tick_ms, freq_hz, cntp, counter_hz },
        }
    reserves 条目格式: { name="...", start=0x..., stop=0x... }
    """
    with open(path, encoding='utf-8') as f:
        text = f.read()
    conf = {}

    # 顶层字段
    conf['ARCH']         = _find_str(text, 'arch')
    conf['MEM_PLATFORM'] = _find_str(text, 'name').upper() or 'QEMU'

    # memory 块
    mem = _extract_block(text, 'memory')
    ram    = _extract_block(mem, 'ram')
    rootfs = _extract_block(mem, 'rootfs')
    conf['MEM_RAM_BASE']    = _find_num(ram,    'base', '0x40000000')
    conf['MEM_RAM_SIZE']    = _find_num(ram,    'size', '0x80000000')
    conf['MEM_ROOTFS_BASE'] = _find_num(rootfs, 'base', '0x60000000')
    conf['MEM_ROOTFS_SIZE'] = _find_num(rootfs, 'size', '0x08000000')
    conf['ROOTFS_SIZE_MB']  = _find_num(rootfs, 'mb',   '128')

    # kernel 块：平台相关的内核链接地址（高半虚拟地址）
    kernel = _extract_block(text, 'kernel')
    conf['KERNEL_LINK_ADDR'] = _find_num(kernel, 'link', '0xffff000040080000')

    # PMM reserves: { name="...", start=0x..., stop=0x... }
    resv_block = _extract_block(mem, 'reserves')
    resv_re = re.compile(
        r'\{\s*name\s*=\s*"([^"]*)"\s*,\s*start\s*=\s*(0x[0-9a-fA-F]+|[0-9]+)'
        r'\s*,\s*stop\s*=\s*(0x[0-9a-fA-F]+|[0-9]+)\s*\}'
    )
    for i, rm in enumerate(resv_re.finditer(resv_block)):
        if i >= 4:
            break
        conf[f'PMM_RESV_{i}_NAME']  = rm.group(1)
        conf[f'PMM_RESV_{i}_START'] = rm.group(2)
        conf[f'PMM_RESV_{i}_END']   = rm.group(3)

    # mmio_vma / lapic
    conf['DEV_MMIO_NEEDS_VMA'] = _find_bool(text, 'mmio_vma')
    conf['DEV_NEED_LAPIC']     = _find_bool(text, 'lapic')

    # uart 块
    uart = _extract_block(text, 'uart')
    conf['DEV_UART_TYPE']      = _find_str(uart, 'driver')
    conf['DEV_UART_BASE']      = _find_num(uart, 'base', '0')
    conf['DEV_UART_REG_SHIFT'] = _find_num(uart, 'reg_shift', '0')

    # irq 块
    irq = _extract_block(text, 'irq')
    conf['DEV_IRQ_TYPE']   = _find_str(irq, 'driver') or 'none'
    conf['DEV_GICD_BASE']  = _find_num(irq, 'gicd',  '0')
    conf['DEV_GICC_BASE']  = _find_num(irq, 'gicc',  '0')
    conf['DEV_GICH_BASE']  = _find_num(irq, 'gich',  '0')
    conf['DEV_GICR_BASE']  = _find_num(irq, 'gicr',  '0')
    conf['DEV_PLIC_BASE']  = _find_num(irq, 'plic',  '0')
    conf['DEV_CLINT_BASE'] = _find_num(irq, 'clint', '0')

    # timer 块
    timer = _extract_block(text, 'timer')
    conf['DEV_TIMER_TYPE']         = _find_str(timer, 'driver')
    conf['DEV_TIMER_TICK_MS']      = _find_num(timer, 'tick_ms',    '10')
    conf['DEV_TIMER_FREQUENCY_HZ'] = _find_num(timer, 'freq_hz',    '100')
    conf['DEV_CNTP_TIMER']         = _find_num(timer, 'cntp',       '0')
    conf['DEV_TIMER_COUNTER_HZ']   = _find_num(timer, 'counter_hz', '0')

    # eth 块
    eth = _extract_block(text, 'eth')
    conf['DEV_ETH_TYPE'] = _find_str(eth, 'driver')
    conf['DEV_ETH_BASE'] = _find_num(eth, 'base', '0')

    # tpu 块
    tpu = _extract_block(text, 'tpu')
    conf['DEV_TPU_TYPE'] = _find_str(tpu, 'driver')
    for key in ('tdma_base', 'tiu_base', 'tdma_irq'):
        val = _find_num(tpu, key, '')
        if val:
            conf[f'DEV_TPU_{key.upper()}'] = val

    # npu 块
    npu = _extract_block(text, 'npu')
    conf['DEV_NPU_TYPE'] = _find_str(npu, 'driver')
    for key in ('base0', 'base1', 'base2', 'pmu', 'irq0', 'irq1', 'irq2'):
        val = _find_num(npu, key, '')
        if val:
            conf[f'DEV_NPU_{key.upper()}'] = val

    sdmmc = _extract_block(text, 'sdmmc')
    for key in ('sd_base', 'top_base', 'top_off_pwrsw_ctrl'):
        val = _find_num(sdmmc, key, '')
        if val:
            conf[f'DEV_SDMMC_{key.upper()}'] = val

    return conf


def to_int(s, fallback=0):
    s = s.strip()
    try:
        if s.startswith('0x') or s.startswith('0X'):
            return int(s, 16)
        return int(s, 10)
    except (ValueError, AttributeError):
        return fallback


def hex_ul(v, bits=32):
    """Format as unsigned hex C literal"""
    if bits == 64 or v > 0xFFFFFFFF:
        return f"0x{v:016X}ULL"
    return f"0x{v:08X}UL"


# ─── 生成 build/platform.mk ──────────────────────────────────────────────────

UART_SRC_MAP = {
    'pl011': 'driver/uart/uart_pl011.c',
    'dw':    'driver/uart/uart_dw.c',
    'x86':   'driver/uart/uart_x86.c',
}

IRQ_SRC_MAP = {
    'gicv2': 'driver/irq/gicv2.c',
    'gicv3': 'driver/irq/gicv3.c',
    'none':  '',
}

GIC_DEFAULT_MAP = {
    'gicv2': 'v2',
    'gicv3': 'v3',
    'none':  'none',
}


def write_platform_mk(conf, path):
    uart_type  = conf.get('DEV_UART_TYPE', '')
    irq_type   = conf.get('DEV_IRQ_TYPE', 'none')
    timer_type = conf.get('DEV_TIMER_TYPE', '')

    uart_src   = UART_SRC_MAP.get(uart_type, '')
    irq_src    = IRQ_SRC_MAP.get(irq_type, '')
    gic_def    = GIC_DEFAULT_MAP.get(irq_type, 'none')
    timer_src  = 'driver/timer/timer.c' if timer_type else ''

    rootfs_base = to_int(conf.get('MEM_ROOTFS_BASE', '0x60000000'))
    rootfs_mb   = conf.get('ROOTFS_SIZE_MB', '128')
    need_lapic  = conf.get('DEV_NEED_LAPIC', '0')
    plat_def    = conf.get('MEM_PLATFORM', 'QEMU')

    content = ''.join([
        "# Auto-generated by tools/gen_platform.py — do not edit\n",
        f"MEM_RAM_BASE        := {conf.get('MEM_RAM_BASE', '0x40000000')}\n",
        f"MEM_RAM_SIZE        := {conf.get('MEM_RAM_SIZE', '0x80000000')}\n",
        f"MEM_ROOTFS_BASE     := {conf.get('MEM_ROOTFS_BASE', '0x60000000')}\n",
        f"MEM_ROOTFS_SIZE     := {conf.get('MEM_ROOTFS_SIZE', '0x08000000')}\n",
        f"KERNEL_LINK_ADDR    := {conf.get('KERNEL_LINK_ADDR', '0xffff000040080000')}\n",
        f"ROOTFS_PHYS_ADDR    := {hex(rootfs_base)}\n",
        f"ROOTFS_SIZE_MB      := {rootfs_mb}\n",
        f"MEM_PLATFORM_DEFINE := {plat_def}\n",
        f"DEV_UART_SRC        := {uart_src}\n",
        f"DEV_IRQ_SRC         := {irq_src}\n",
        f"DEV_TIMER_SRC       := {timer_src}\n",
        f"DEV_MMIO_NEEDS_VMA  := {conf.get('DEV_MMIO_NEEDS_VMA', '0')}\n",
        f"DEV_UART_BASE_RAW   := {conf.get('DEV_UART_BASE', '0')}\n",
        f"DEV_UART_REG_SHIFT  := {conf.get('DEV_UART_REG_SHIFT', '0')}\n",
        f"DEV_DEFAULT_UART    := {uart_type}\n",
        f"DEV_DEFAULT_GIC     := {gic_def}\n",
        f"DEV_NEED_LAPIC      := {need_lapic}\n",
        f"DEV_ETH_TYPE        := {conf.get('DEV_ETH_TYPE', '')}\n",
        f"DEV_ETH_BASE        := {conf.get('DEV_ETH_BASE', '0')}\n",
        f"DEV_TPU_TYPE        := {conf.get('DEV_TPU_TYPE', '')}\n",
        f"DEV_NPU_TYPE        := {conf.get('DEV_NPU_TYPE', '')}\n",
    ])
    write_if_changed(path, content)


def _cstr(s):
    return s.replace('\\', '\\\\').replace('"', '\\"')


def write_platform_static_c(conf, build_dir):
    entries = []

    def add(block, key, value):
        entries.append((block, key, value))

    add('memory.ram', 'base', conf.get('MEM_RAM_BASE', '0'))
    add('memory.ram', 'size', conf.get('MEM_RAM_SIZE', '0'))
    add('memory.rootfs', 'base', conf.get('MEM_ROOTFS_BASE', '0'))
    add('memory.rootfs', 'size', conf.get('MEM_ROOTFS_SIZE', '0'))
    add('platform', 'mmio_vma', conf.get('DEV_MMIO_NEEDS_VMA', '0'))
    add('uart', 'base', conf.get('DEV_UART_BASE', '0'))
    add('uart', 'reg_shift', conf.get('DEV_UART_REG_SHIFT', '0'))
    add('irq', 'gicd', conf.get('DEV_GICD_BASE', '0'))
    add('irq', 'gicc', conf.get('DEV_GICC_BASE', '0'))
    add('irq', 'gich', conf.get('DEV_GICH_BASE', '0'))
    add('irq', 'gicr', conf.get('DEV_GICR_BASE', '0'))
    add('irq', 'plic', conf.get('DEV_PLIC_BASE', '0'))
    add('irq', 'clint', conf.get('DEV_CLINT_BASE', '0'))
    add('timer', 'freq_hz', conf.get('DEV_TIMER_FREQUENCY_HZ', '100'))
    add('timer', 'tick_ms', conf.get('DEV_TIMER_TICK_MS', '10'))
    add('timer', 'counter_hz', conf.get('DEV_TIMER_COUNTER_HZ', '0'))
    add('timer', 'cntp', conf.get('DEV_CNTP_TIMER', '0'))
    add('eth', 'base', conf.get('DEV_ETH_BASE', '0'))

    for key in ('base0', 'base1', 'base2', 'pmu', 'irq0', 'irq1', 'irq2'):
        value = conf.get(f'DEV_NPU_{key.upper()}')
        if value is not None:
            add('npu', key, value)
    for key in ('tdma_base', 'tiu_base', 'tdma_irq'):
        value = conf.get(f'DEV_TPU_{key.upper()}')
        if value is not None:
            add('tpu', key, value)
    for key in ('sd_base', 'top_base', 'top_off_pwrsw_ctrl'):
        value = conf.get(f'DEV_SDMMC_{key.upper()}')
        if value is not None:
            add('sdmmc', key, value)

    lines = [
        '/* Auto-generated by tools/gen_platform.py — do not edit */\n',
        '#include "platform_cfg.h"\n\n',
        'const platform_kv_t g_platform_static_kv[] = {\n',
    ]
    for block, key, value in entries:
        lines.append(f'    {{ "{_cstr(block)}", "{_cstr(key)}", (uintptr_t)({value}) }},\n')
    lines.append('};\n')
    lines.append(f'const unsigned g_platform_static_kv_count = {len(entries)}u;\n\n')
    lines.append('const platform_reserve_t g_platform_static_reserves[] = {\n')
    reserve_count = 0
    for i in range(4):
        name = conf.get(f'PMM_RESV_{i}_NAME', '')
        start = conf.get(f'PMM_RESV_{i}_START', '')
        end = conf.get(f'PMM_RESV_{i}_END', '')
        if name and start and end:
            lines.append(f'    {{ "{_cstr(name)}", (uint64_t)({start}), (uint64_t)({end}) }},\n')
            reserve_count += 1
    lines.append('};\n')
    lines.append(f'const unsigned g_platform_static_reserve_count = {reserve_count}u;\n')
    write_if_changed(os.path.join(build_dir, 'platform_static.c'), ''.join(lines))


# ─── 生成 include/mem_layout.h ───────────────────────────────────────────────

def write_mem_layout_h(conf, path):
    ram_base    = to_int(conf.get('MEM_RAM_BASE',    '0x40000000'))
    ram_size    = to_int(conf.get('MEM_RAM_SIZE',    '0x80000000'))
    rootfs_base = to_int(conf.get('MEM_ROOTFS_BASE', '0x60000000'))
    rootfs_size = to_int(conf.get('MEM_ROOTFS_SIZE', '0x08000000'))

    with open(path, 'w') as f:
        f.write("/* Auto-generated by tools/gen_platform.py — do not edit */\n")
        f.write("#ifndef MEM_LAYOUT_H\n#define MEM_LAYOUT_H\n\n")
        f.write(f"#define MEM_RAM_BASE      {hex_ul(ram_base)}\n")
        f.write(f"#define MEM_RAM_SIZE      {hex_ul(ram_size)}\n")
        f.write(f"#define MEM_ROOTFS_BASE   {hex_ul(rootfs_base)}\n")
        f.write(f"#define MEM_ROOTFS_SIZE   {hex_ul(rootfs_size)}\n")
        f.write("#define MEM_LAYOUT_VALID  1\n\n")
        f.write("#endif /* MEM_LAYOUT_H */\n")


# ─── 生成 include/device_profile.h ───────────────────────────────────────────

def write_device_profile_h(conf, path):
    uart  = conf.get('DEV_UART_TYPE',  '')
    irq   = conf.get('DEV_IRQ_TYPE',   'none')
    timer = conf.get('DEV_TIMER_TYPE', '')

    uart_pl011 = 1 if uart  == 'pl011' else 0
    uart_dw    = 1 if uart  == 'dw'    else 0
    uart_x86   = 1 if uart  == 'x86'   else 0
    gic_v2     = 1 if irq   == 'gicv2' else 0
    gic_v3     = 1 if irq   == 'gicv3' else 0
    t_a64      = 1 if timer == 'aarch64' else 0
    t_rv       = 1 if timer == 'rv'     else 0
    t_x86      = 1 if timer == 'x86'    else 0

    mmio_vma   = conf.get('DEV_MMIO_NEEDS_VMA', '0')
    uart_base  = to_int(conf.get('DEV_UART_BASE',  '0'))
    gicd_base  = to_int(conf.get('DEV_GICD_BASE',  '0'))
    gicc_base  = to_int(conf.get('DEV_GICC_BASE',  '0'))
    gich_base  = to_int(conf.get('DEV_GICH_BASE',  '0'))
    gicr_base  = to_int(conf.get('DEV_GICR_BASE',  '0'))
    plic_base  = to_int(conf.get('DEV_PLIC_BASE',  '0'))
    clint_base = to_int(conf.get('DEV_CLINT_BASE', '0'))
    cntp       = conf.get('DEV_CNTP_TIMER',        '0')
    reg_shift  = conf.get('DEV_UART_REG_SHIFT',    '0')
    tick_ms    = conf.get('DEV_TIMER_TICK_MS',      '10')
    freq_hz    = conf.get('DEV_TIMER_FREQUENCY_HZ', '100')
    counter_hz = to_int(conf.get('DEV_TIMER_COUNTER_HZ', '0'))

    with open(path, 'w') as f:
        f.write("/* Auto-generated by tools/gen_platform.py — do not edit */\n")
        f.write("#ifndef DEVICE_PROFILE_H\n#define DEVICE_PROFILE_H\n\n")
        f.write(f"#define DEVICE_PROFILE_VALID          1\n")
        f.write(f"#define DEVICE_MMIO_NEEDS_VMA         {mmio_vma}\n")
        f.write(f"#define DEVICE_DEFAULT_UART_PL011     {uart_pl011}\n")
        f.write(f"#define DEVICE_DEFAULT_UART_DW        {uart_dw}\n")
        f.write(f"#define DEVICE_DEFAULT_UART_X86       {uart_x86}\n")
        f.write(f"#define DEVICE_DEFAULT_GIC_V2         {gic_v2}\n")
        f.write(f"#define DEVICE_DEFAULT_GIC_V3         {gic_v3}\n")
        f.write(f"#define DEVICE_DEFAULT_TIMER_AARCH64  {t_a64}\n")
        f.write(f"#define DEVICE_DEFAULT_TIMER_RV       {t_rv}\n")
        f.write(f"#define DEVICE_DEFAULT_TIMER_X86      {t_x86}\n")
        f.write(f"#define DEVICE_UART_BASE_RAW          {hex_ul(uart_base)}\n")
        f.write(f"#define DEVICE_GICD_BASE_RAW          {hex_ul(gicd_base)}\n")
        f.write(f"#define DEVICE_GICC_BASE_RAW          {hex_ul(gicc_base)}\n")
        f.write(f"#define DEVICE_GICH_BASE_RAW          {hex_ul(gich_base)}\n")
        f.write(f"#define DEVICE_GICR_BASE_RAW          {hex_ul(gicr_base)}\n")
        f.write(f"#define DEVICE_PLIC_BASE_RAW          {hex_ul(plic_base)}\n")
        f.write(f"#define DEVICE_CLINT_BASE_RAW         {hex_ul(clint_base)}\n")
        f.write(f"#define DEVICE_TIMER_TICK_MS          {tick_ms}\n")
        f.write(f"#define DEVICE_TIMER_FREQUENCY_HZ     {freq_hz}\n")
        f.write(f"#define DEVICE_TIMER_COUNTER_HZ       {hex_ul(counter_hz)}\n")
        f.write(f"#define DEVICE_CNTP_TIMER             {cntp}\n")
        f.write(f"#define DEVICE_UART_REG_SHIFT         {reg_shift}\n")
        f.write("\n#endif /* DEVICE_PROFILE_H */\n")


# ─── 生成 include/pmm_reserve.h ──────────────────────────────────────────────

def write_pmm_reserve_h(conf, path):
    reserves = []
    for i in range(4):
        name  = conf.get(f'PMM_RESV_{i}_NAME',  '')
        start = conf.get(f'PMM_RESV_{i}_START', '')
        end   = conf.get(f'PMM_RESV_{i}_END',   '')
        if name and start and end:
            reserves.append({
                'name':  name,
                'start': to_int(start),
                'end':   to_int(end),
            })

    with open(path, 'w') as f:
        f.write("/* Auto-generated by tools/gen_platform.py — do not edit */\n")
        f.write("#ifndef PMM_RESERVE_H\n#define PMM_RESERVE_H\n\n")
        for i in range(2):
            if i < len(reserves):
                r = reserves[i]
                f.write(f"#define PMM_EXTRA_RESV{i}_ENABLE  1\n")
                f.write(f'#define PMM_EXTRA_RESV{i}_TAG     "{r["name"]}"\n')
                f.write(f"#define PMM_EXTRA_RESV{i}_START   {hex_ul(r['start'], 64)}\n")
                f.write(f"#define PMM_EXTRA_RESV{i}_END     {hex_ul(r['end'],   64)}\n")
            else:
                f.write(f"#define PMM_EXTRA_RESV{i}_ENABLE  0\n")
                f.write(f'#define PMM_EXTRA_RESV{i}_TAG     ""\n')
                f.write(f"#define PMM_EXTRA_RESV{i}_START   0ULL\n")
                f.write(f"#define PMM_EXTRA_RESV{i}_END     0ULL\n")
        f.write("\n#endif /* PMM_RESERVE_H */\n")


# ─── main ─────────────────────────────────────────────────────────────────────

def main():
    if len(sys.argv) < 4:
        print(f"Usage: {sys.argv[0]} <platform.conf> <build/platform.mk> <include/>")
        sys.exit(1)

    conf_path   = sys.argv[1]
    mk_path     = sys.argv[2]
    include_dir = sys.argv[3]
    build_dir   = os.path.dirname(mk_path)

    os.makedirs(build_dir, exist_ok=True)
    os.makedirs(include_dir, exist_ok=True)

    conf = parse_platform_conf(conf_path)

    write_platform_mk(conf, mk_path)
    # 以下 3 个头文件已由运行时 platform_cfg.h / platform_cfg.c 取代，不再生成：
    #   write_mem_layout_h(conf,   os.path.join(include_dir, 'mem_layout.h'))
    #   write_device_profile_h(conf, os.path.join(include_dir, 'device_profile.h'))
    #   write_pmm_reserve_h(conf,  os.path.join(include_dir, 'pmm_reserve.h'))
    write_platform_static_c(conf, build_dir)


if __name__ == '__main__':
    main()
