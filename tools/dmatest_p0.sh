#!/usr/bin/env bash
#
# dmatest_p0.sh — dma-buf / dma-heap（P0）的端到端验收
#
# 用法：
#     tools/dmatest_p0.sh [arch]          # 默认 aarch64
#
# 前置（**变体标志必须一致**，否则内核会被悄悄换成非 DMA 变体 —— 见 CLAUDE.md）：
#     make PLATFORM=qemu-virt-aarch64 LOG=info DMA=1 clean
#     make PLATFORM=qemu-virt-aarch64 LOG=info DMA=1 kernel rootfs
#
# 判据：宿主机上跑 /bin/dmatest，要求它自己报出 0 失败。
# 驱动 QEMU 的写法照 tools/vmm_multivm_regress.sh（mkfifo + 保持写端打开）。
set -u

ARCH=${1:-aarch64}
PLAT=qemu-virt-$ARCH
K=build/$PLAT/kernel_${ARCH}.bin
R=build/$PLAT/rootfs-$ARCH.img
RA=0x5fe00000            # build/$PLAT/platform.mk 的 ROOTFS_PHYS_ADDR（aarch64）
Q=qemu-system-$ARCH

case "$ARCH" in
aarch64)
    QARGS=(-cpu cortex-a76 -M virt,virtualization=on,gic-version=3)
    ;;
riscv64)
    QARGS=(-M virt -bios default)
    ;;
*)
    echo "仅支持 aarch64 / riscv64（QEMU 参数字典没配 $ARCH）"
    exit 1
    ;;
esac

[ -f "$K" ] || { echo "缺 $K"; exit 1; }
[ -f "$R" ] || { echo "缺 $R"; exit 1; }

# 产物必须是 DMA 变体，否则节点根本不存在，症状是"open 失败"而看不出原因
if ! strings "$K" 2>/dev/null | grep -q 'dma_heap'; then
    echo "❌ $K 里没有 dma-heap 的字符串 —— 这不是 DMA=1 的构建"
    echo "   重建：make PLATFORM=$PLAT LOG=info DMA=1 clean && make PLATFORM=$PLAT LOG=info DMA=1 kernel rootfs"
    exit 1
fi

OUT=/tmp/dmatest_p0_$ARCH
mkdir -p "$OUT"
LOG=$OUT/session.log
FIFO=$OUT/.stdin.$$
rm -f "$LOG" "$FIFO"
mkfifo "$FIFO"

# shellcheck disable=SC2086
timeout 300 $Q "${QARGS[@]}" -smp 1 -m 2G -nographic -kernel "$K" \
    -device loader,file="$R",addr=$RA,force-raw=on \
    < "$FIFO" > "$LOG" 2>&1 &
QPID=$!
exec 3<> "$FIFO"          # 保持写端打开，否则 QEMU 立刻看到 EOF
cleanup() { exec 3>&-; kill $QPID 2>/dev/null; wait $QPID 2>/dev/null; }
trap cleanup EXIT

# wait_count <模式> <次数> <超时秒>   固定串匹配（默认 -F）
wait_count() {
    local pat=$1 want=$2 tmo=$3 i=0 lim
    lim=$((tmo * 5))
    while [ "$i" -lt "$lim" ]; do
        local n
        n=$(grep -acF -- "$pat" "$LOG" 2>/dev/null)
        case "$n" in ''|*[!0-9]*) n=0 ;; esac
        [ "$n" -ge "$want" ] && return 0
        sleep 0.2
        i=$((i + 1))
    done
    return 1
}

echo "== dma-buf / dma-heap (P0) 验收 · $ARCH =="

if ! wait_count 'Launching busybox shell' 1 180; then
    echo "❌ 宿主 shell 没起来（看 $LOG）"
    exit 1
fi
echo "   宿主就绪"

printf '/bin/dmatest\n' >&3

if wait_count '=== 结果:' 1 120; then
    echo
    sed -n '/=== dmatest: dma-buf/,/=== 结果:/p' "$LOG"
    if grep -aFq '0 失败' "$LOG"; then
        echo "✅ P0 验收通过"
        exit 0
    fi
    echo "❌ 有用例失败（完整日志：$LOG）"
    exit 1
fi

echo "❌ dmatest 没有跑完（完整日志：$LOG）"
tail -30 "$LOG"
exit 1
