#!/usr/bin/env bash
#
# guest_npu_qemu_test.sh — 在 QEMU 里验证 **guest 侧**的 NPU 链路
#
# 不需要 RK3588 硬件，也不需要宿主 Avatar OS —— 直接拿 guest 内核 + DTB +
# initramfs 在 QEMU virt 上起，检查 rknpu.ko 能不能 probe、预留 DMA 池能不能
# 建起来。**真机上那两轮排查都被"DTB 里少一个 no-map"卡住过，而这条在
# QEMU 里就能一眼看出来**（见下），所以值得有个脚本。
#
# 它**测不了**的：NPU 寄存器访问和作业执行（QEMU 没有 NPU 硬件）。
# 那部分只能在真机上验。
#
# 判据（三条，缺一不可）：
#   ① `RKNPU: initialize reserved memory for rknpu device!`
#        驱动确认它的 DMA 池建起来了。**缺这条 = no-map 丢了。**
#   ② `[drm] Initialized rknpu ... on minor 0`
#        驱动 probe 完成。
#   ③ 没有 `memremap attempted on ram`
#        那行看着像啰嗦，实际是 memremap 对 System RAM 用 WC 时 **return NULL**
#        （kernel/iomem.c:106-110），也就是 ① 失败的根因。
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
IMAGE=${IMAGE:-$HOME/kbuild/linux-guest-arm64/arch/arm64/boot/Image}
DTB=${DTB:-$ROOT/imgs/guests/aarch64/linux-gicv3-npu.dtb}
INITRD=${INITRD:-$ROOT/imgs/guests/aarch64/initrd.gz}
SECS=${SECS:-60}

show() { printf '  %-46s %s\n' "$1" "$2"; }

for f in "$IMAGE" "$DTB" "$INITRD"; do
    [ -f "$f" ] || { echo "缺 $f" >&2; exit 1; }
done

# ⚠️ 这个 DTB 是给 **avatar VMM** 用的：内存节点在 0x70000000。而 QEMU 把内核
#    放在 RAM 起始 +0x80000（默认 0x40080000）—— 内存图里没有内核自己，
#    内核会在 start_kernel 之前就崩掉，**一个字都不输出**（我第一次就栽在这）。
#    所以临时把内存节点挪到 QEMU 的位置。保留区 0x79000000 仍在 1 GiB 内 ✓。
TMP=$(mktemp -d /tmp/gnpu-qemu.XXXXXX)
trap 'rm -rf "$TMP"' EXIT

# ── 先查 DTB 的两个属性（不用起 QEMU，这是最常错的一环）────────────────
#
# ⚠️ **必须按节点查。** 只查"属性名在不在整份 DTB 里"是没用的：
#    `dma-coherent` 在 QEMU virt 的 fw-cfg / virtio_mmio / pcie 三个节点上
#    本来就有，那种查法永远通过。真踩过这个假阳性。
prop_check() { # $1=节点名 $2=属性名
    python3 - "$DTB" "$1" "$2" <<'PY'
import re, subprocess, sys
dtb, node, prop = sys.argv[1], sys.argv[2], sys.argv[3]
dts = subprocess.run(["dtc", "-I", "dtb", "-O", "dts", dtb],
                     capture_output=True, text=True).stdout
# 取该节点的花括号块，再在里面找属性
m = re.search(r"^\s*%s\s*\{" % re.escape(node), dts, re.M)
if not m:
    print("NODE_MISSING"); raise SystemExit
i, depth = m.end(), 1
while i < len(dts) and depth:
    if dts[i] == "{": depth += 1
    elif dts[i] == "}": depth -= 1
    i += 1
print("YES" if re.search(r"\b%s\b" % re.escape(prop), dts[m.end():i]) else "NO")
PY
}

echo "═══ DTB 属性 ═══"
bad=0
for spec in "npu-pool@79000000:no-map" "npu@fdab0000:dma-coherent"; do
    n=${spec%%:*}; p=${spec##*:}
    r=$(prop_check "$n" "$p")
    case "$r" in
        YES) show "$n 有 $p" "✓" ;;
        NO)  show "$n 有 $p" "✗ —— 会被下面第 ①/② 条抓到"; bad=1 ;;
        *)   show "$n 存在" "✗ —— 这份 DTB 不是 NPU 版（要用 GUEST_NPU=1）"; bad=1 ;;
    esac
done
[ $bad -eq 0 ] || { echo; echo "先修 DTB，不用往下跑。"; exit 1; }

dtc -I dtb -O dts "$DTB" 2>/dev/null \
    | sed 's/memory@70000000/memory@40000000/;
           s/reg = <0x00 0x70000000 0x00 0xc000000>/reg = <0x00 0x40000000 0x00 0x40000000>/' \
    | dtc -I dts -O dtb -o "$TMP/guest.dtb" 2>/dev/null

LOG=$TMP/log
# ⚠️ `gic-version=3` 不能省：QEMU virt 默认给 GICv2，而这个 DTB 写的是
#    arm,gic-v3 —— 不匹配时 guest 在 gic_of_init 里撞外部中止然后 panic。
timeout "$SECS" qemu-system-aarch64 -M virt,gic-version=3 -cpu max -m 1024 \
    -nographic -no-reboot \
    -kernel "$IMAGE" -initrd "$INITRD" -dtb "$TMP/guest.dtb" \
    -append "console=ttyAMA0 earlycon=pl011,0x9000000 rdinit=/init" \
    >"$LOG" 2>&1 || true

echo "═══ guest 侧 NPU 链路自检 ═══"
rc=0
if grep -q 'initialize reserved memory for rknpu device' "$LOG"; then
    show "① 预留 DMA 池建起来了" "✓"
else
    show "① 预留 DMA 池建起来了" "✗ —— 多半是 DTB 里少了 no-map"; rc=1
fi
if grep -q 'Initialized rknpu' "$LOG"; then
    show "② rknpu probe 完成" "✓"
else
    show "② rknpu probe 完成" "✗"; rc=1
fi
if grep -q 'memremap attempted on ram' "$LOG"; then
    show "③ 没有 memremap(WC) 打在 System RAM 上" "✗ —— 就是 ① 的根因"; rc=1
else
    show "③ 没有 memremap(WC) 打在 System RAM 上" "✓"
fi

[ $rc -eq 0 ] || { echo; echo "完整日志：$LOG"; cp "$LOG" /tmp/gnpu-qemu-last.log; echo "（已留一份在 /tmp/gnpu-qemu-last.log）"; }
exit $rc
