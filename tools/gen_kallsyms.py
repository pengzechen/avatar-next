#!/usr/bin/env python3
"""
gen_kallsyms.py  —  从链接好的内核 ELF 抽出函数符号表，生成可链接的 .S

用法:
  # 生成符号表（stage-1 ELF → .S）
  python3 tools/gen_kallsyms.py <stage1.elf> <out.S>

  # 校验：确认 <final.elf> 里 .text 段所有函数符号的地址和 stage-1 完全一致
  python3 tools/gen_kallsyms.py --verify <stage1.elf> <final.elf>

为什么是两步：内核用 `objcopy -O binary` 出 .bin 再交给 QEMU `-kernel`，
镜像里没有符号表；要打出 `func+0x12` 就必须把符号表编进内核。
而符号表的内容依赖链接结果（地址），所以只能「先链接一遍拿地址 → 生成表 →
再链接一遍带上表」。

为什么地址不会漂移：.kallsyms 段被 link.ld 放在 .rodata 之后、.data 之前，
而 .text 排在它前面，所以新增这一段不移动任何 .text 里的东西。
表里也只放 STT_FUNC（.text 内）—— 数据符号的地址在两趟之间确实会变，
不能收进来。--verify 就是给这条不变量兜底的：一旦有人把 .kallsyms 挪到
.text 前面，或者挪动段序，构建会当场失败而不是产出一张错位的符号表。

（更省空间的 Linux 做法是 token 压缩名字，1685 个符号大约能省一半。
这里不做：多出来的解码逻辑换几十 KB，对 freestanding 内核不划算。）
"""

import sys
import struct

# ─── ELF64 常量 ──────────────────────────────────────────────────────────────

SHT_SYMTAB = 2
STT_FUNC = 2
SHN_UNDEF = 0
SHN_XINDEX = 0xFFFF

# ─── ELF64 解析 ──────────────────────────────────────────────────────────────
#
# 只解析需要的那几个字段，不引第三方库（三个架构的交叉 ELF 都要能读，
# 而 pyelftools 不一定装在每个人的机器上，readelf 的列格式又会随版本变）。


class Elf:
    def __init__(self, path):
        with open(path, 'rb') as f:
            self.data = f.read()

        if self.data[:4] != b'\x7fELF':
            raise ValueError(f'{path}: 不是 ELF 文件')
        if self.data[4] != 2:
            raise ValueError(f'{path}: 不是 64 位 ELF（EI_CLASS={self.data[4]}）')

        (self.e_shoff,) = struct.unpack_from('<Q', self.data, 0x28)
        (self.e_shentsize, self.e_shnum, self.e_shstrndx) = \
            struct.unpack_from('<HHH', self.data, 0x3A)

        self._read_sections()

    def _read_sections(self):
        self.sections = []
        for i in range(self.e_shnum):
            off = self.e_shoff + i * self.e_shentsize
            (name, stype, _flags, addr, sh_offset, size, link, _info,
             _align, entsize) = struct.unpack_from('<IIQQQQIIQQ', self.data, off)
            self.sections.append({
                'name': name, 'type': stype, 'addr': addr, 'offset': sh_offset,
                'size': size, 'link': link, 'entsize': entsize,
            })

        # 段名表
        strtab = self.sections[self.e_shstrndx]
        base = strtab['offset']
        for s in self.sections:
            end = self.data.index(b'\0', base + s['name'])
            s['str'] = self.data[base + s['name']:end].decode('utf-8', 'replace')

    def symtab(self):
        """返回 [(name, value, info, shndx), ...]，读不到符号表时返回空表。"""
        sec = next((s for s in self.sections if s['type'] == SHT_SYMTAB), None)
        if sec is None:
            return []

        strtab = self.sections[sec['link']]
        sbase = strtab['offset']

        out = []
        count = sec['size'] // 24
        for i in range(count):
            off = sec['offset'] + i * 24
            (st_name, st_info, _other, st_shndx, st_value, _size) = \
                struct.unpack_from('<IBBHQQ', self.data, off)
            if st_name == 0:
                continue
            end = self.data.index(b'\0', sbase + st_name)
            name = self.data[sbase + st_name:end].decode('utf-8', 'replace')
            out.append((name, st_value, st_info, st_shndx))
        return out

    def text_bounds(self, syms):
        """[_stext, _etext)。优先用链接脚本导出的符号，缺失时退到 .text 段本身。"""
        by_name = {n: v for (n, v, _i, _s) in syms}
        if '_stext' in by_name and '_etext' in by_name:
            return by_name['_stext'], by_name['_etext']

        sec = next((s for s in self.sections if s['str'] == '.text'), None)
        if sec is None:
            raise ValueError('找不到 _stext/_etext，也没有 .text 段')
        return sec['addr'], sec['addr'] + sec['size']


def collect_functions(path):
    """返回按地址排序、去重后的 [(addr, name), ...]，只含 .text 内的 STT_FUNC。"""
    elf = Elf(path)
    syms = elf.symtab()
    if not syms:
        raise ValueError(f'{path}: 没有 .symtab —— 链接时是不是被 strip 了？')

    lo, hi = elf.text_bounds(syms)

    seen = {}
    for (name, value, info, shndx) in syms:
        if (info & 0xF) != STT_FUNC:
            continue
        if shndx == SHN_UNDEF or shndx == SHN_XINDEX:
            continue
        if not (lo <= value < hi):
            continue
        # 同一地址可能有多个别名（local/global）；按名字取字典序最小的，
        # 保证同样的输入永远产出同样的表。
        if value not in seen or name < seen[value]:
            seen[value] = name

    return sorted(seen.items())


# ─── .S 生成 ─────────────────────────────────────────────────────────────────


def gas_string(raw):
    """把 bytes 编成合法的 GAS 字符串字面量（含结尾 NUL）。"""
    out = ['"']
    for b in raw + b'\0':
        if b in (0x22, 0x5C):            # " 和 \
            out.append('\\' + chr(b))
        elif 0x20 <= b < 0x7F:
            out.append(chr(b))
        else:
            out.append('\\%03o' % b)
    out.append('"')
    return ''.join(out)


def emit(elf_path, out_path, funcs):
    names_blob = bytearray()
    name_off = []
    for _addr, name in funcs:
        name_off.append(len(names_blob))
        names_blob += name.encode('utf-8') + b'\0'

    lines = []
    lines.append('/* 由 tools/gen_kallsyms.py 生成 —— 不要手工编辑。')
    lines.append(' * 源: %s' % elf_path)
    lines.append(' * %d 个函数符号。' % len(funcs))
    lines.append(' *')
    lines.append(' * 段序要求：link.ld 必须把 .kallsyms 放在 .text 之后。')
    lines.append(' * 表里的地址取自「不带本表」的那一趟链接，只有 .text 不动')
    lines.append(' * 它们才继续有效；构建后的 --verify 会校验这一点。 */')
    lines.append('')
    lines.append('\t.section .kallsyms,"a",@progbits')
    lines.append('\t.balign 8')
    lines.append('\t.globl __kallsyms_count')
    lines.append('__kallsyms_count:')
    lines.append('\t.long %d' % len(funcs))
    lines.append('')
    lines.append('\t.balign 8')
    lines.append('\t.globl __kallsyms_addrs')
    lines.append('__kallsyms_addrs:')
    for addr, _name in funcs:
        lines.append('\t.quad 0x%016x' % addr)
    lines.append('')
    lines.append('\t.balign 4')
    lines.append('\t.globl __kallsyms_name_off')
    lines.append('__kallsyms_name_off:')
    for off in name_off:
        lines.append('\t.long %d' % off)
    lines.append('')
    lines.append('\t.globl __kallsyms_names')
    lines.append('__kallsyms_names:')
    # 每行一个名字，方便人肉 grep 这张表
    for _addr, name in funcs:
        lines.append('\t.ascii %s' % gas_string(name.encode('utf-8')))
    lines.append('')

    return '\n'.join(lines)


def verify(stage1_path, final_path):
    """确认两趟链接里 (地址, 函数名) 集合完全一致。不一致 = 符号表已错位。

    按集合比对而不是按名字建字典：同名 static 函数可以在不同编译单元里
    各定义一份（实测 x86_64 内核有 3 个），按名字做 key 会让后一个覆盖
    前一个，从而漏检真正的地址漂移。
    """
    a = set(collect_functions(stage1_path))
    b = set(collect_functions(final_path))

    if a != b:
        sys.stderr.write(
            'gen_kallsyms: 符号表已错位 —— stage-1 与最终 ELF 的函数地址不一致。\n'
            '  这会让 panic backtrace 打出错误的函数名。\n'
            '  最可能的原因：link.ld 里 .kallsyms 段被放到了 .text 之前。\n')

        by_addr_a = dict(a)
        by_addr_b = dict(b)

        drifted = [(x, by_addr_a[x], by_addr_b[x])
                   for x in by_addr_a.keys() & by_addr_b.keys()
                   if by_addr_a[x] != by_addr_b[x]]
        for (addr, n1, n2) in drifted[:10]:
            sys.stderr.write('    0x%x: %s -> %s\n' % (addr, n1, n2))

        for (addr, n) in sorted(a - b)[:10]:
            sys.stderr.write('    stage-1 有而最终 ELF 没有: %s (0x%x)\n' % (n, addr))
        for (addr, n) in sorted(b - a)[:10]:
            sys.stderr.write('    最终 ELF 多出: %s (0x%x)\n' % (n, addr))
        return 1

    sys.stdout.write('  [kallsyms] 校验通过：%d 个函数符号地址一致\n' % len(a))
    return 0


def emit_stub():
    """stage-1 用的空表：四个符号都定义，但 count=0。

    ⚠️ 这段的**对齐必须和 emit() 逐项一致**，不是随手写的。原因：
      - aarch64 用 `ldr x, [x, #:lo12:sym]` 取数组基址，这条指令要求符号
        8 字节对齐。桩里 `__kallsyms_addrs` 没对齐的话，链接器只能改用
        adrp+add+ldr 三条指令，报 R_AARCH64_LDST64_ABS_LO12_NC 截断。
      - 就算能链过，只要两趟的寻址形态不一样，.text 就会差几个字节，
        两趟地址对不上，符号表全错位。

    为什么 stage-1 需要一个**真的定义了这些符号**的桩，而不是干脆不链接、
    或者让 C 侧用弱符号兜底 —— 实测踩过：弱符号在 stage-1 里是"未定义"
    （链接器把地址置 0），C 侧取地址比较就变成了"构造常量 0"，需要比
    PC 相对寻址更长的指令序列；stage-2 里符号有真地址，走短序列。
    结果是 backtrace_lookup() 在两趟里差了 16 字节，从它往后的所有函数
    地址整体偏移（riscv64 上实测）。
    """
    lines = ['/* 由 tools/gen_kallsyms.py --stub 生成 —— stage-1 用的空符号表。',
             ' * 对齐和真表一致，且四个符号都是真定义，理由见本脚本 emit_stub()。 */',
             '',
             '\t.section .kallsyms,"a",@progbits',
             '\t.balign 8',
             '\t.globl __kallsyms_count',
             '__kallsyms_count:',
             '\t.long 0',
             '',
             '\t.balign 8',
             '\t.globl __kallsyms_addrs',
             '__kallsyms_addrs:',
             '\t.quad 0',
             '',
             '\t.balign 4',
             '\t.globl __kallsyms_name_off',
             '__kallsyms_name_off:',
             '\t.long 0',
             '',
             '\t.globl __kallsyms_names',
             '__kallsyms_names:',
             '\t.byte 0',
             '']
    return '\n'.join(lines)


def main(argv):
    if len(argv) == 4 and argv[1] == '--verify':
        return verify(argv[2], argv[3])

    if len(argv) == 3 and argv[1] == '--stub':
        with open(argv[2], 'w') as f:
            f.write(emit_stub())
        return 0

    if len(argv) != 3:
        sys.stderr.write(__doc__)
        return 2

    elf_path, out_path = argv[1], argv[2]
    funcs = collect_functions(elf_path)
    text = emit(elf_path, out_path, funcs)

    # 只在内容变化时写回：避免无谓地刷新 mtime，让 make 反复重链。
    try:
        with open(out_path, 'r') as f:
            if f.read() == text:
                return 0
    except FileNotFoundError:
        pass

    with open(out_path, 'w') as f:
        f.write(text)

    sys.stdout.write('  [kallsyms] %d 个函数符号 -> %s\n' % (len(funcs), out_path))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
