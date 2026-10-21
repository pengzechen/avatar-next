#!/bin/bash
# task_completion_test.sh — completion 模块的用户态 A/B 验收
#
# 需要 TASK_TEST=1 内核（暴露 completion wait/complete/reset 测试 syscall）
# 和 rootfs 里的 /bin/task-completion。
#
# 用法：
#   make PLATFORM=<p> SMP=2 TASK_TEST=1 clean
#   make PLATFORM=<p> SMP=2 TASK_TEST=1 kernel rootfs
#   tools/task_completion_test.sh <arch> [smp]
#
# 环境变量 KERNEL_BIN / ROOTFS_IMG 可指向基线内核做对照。
set -u
cd "$(dirname "$0")/.." || exit 1

ARCH=${1:-x86_64}
SMPV=${2:-2}
OUT=${OUT:-/tmp/task_completion_$ARCH.log}

case "$ARCH" in
  x86_64)
    Q="qemu-system-x86_64 -machine microvm -enable-kvm -cpu host"
    K=${KERNEL_BIN:-build/qemu-virt-x86_64/kernel_x86_64.bin}
    R=${ROOTFS_IMG:-build/qemu-virt-x86_64/rootfs-x86_64.img}
    RA=0x4000000 ;;
  aarch64)
    Q="qemu-system-aarch64 -cpu cortex-a76 -M virt,virtualization=on,gic-version=3"
    K=${KERNEL_BIN:-build/qemu-virt-aarch64/kernel_aarch64.bin}
    R=${ROOTFS_IMG:-build/qemu-virt-aarch64/rootfs-aarch64.img}
    RA=0x5fe00000 ;;
  riscv64)
    Q="qemu-system-riscv64 -M virt -bios default"
    K=${KERNEL_BIN:-build/qemu-virt-riscv64/kernel_riscv64.bin}
    R=${ROOTFS_IMG:-build/qemu-virt-riscv64/rootfs-riscv64.img}
    RA=0x88000000 ;;
  *) echo "用法: $0 <x86_64|aarch64|riscv64> [smp]"; exit 1 ;;
esac

[ -f "$K" ] || { echo "缺 $K"; exit 1; }
[ -f "$R" ] || { echo "缺 $R"; exit 1; }

FIFO=/tmp/.task_completion_$$.fifo
rm -f "$FIFO" "$OUT"
mkfifo "$FIFO"

# shellcheck disable=SC2086
timeout 420 $Q -smp "$SMPV" -m 2G -nographic -kernel "$K" \
    -device loader,file="$R",addr="$RA",force-raw=on \
    < "$FIFO" > "$OUT" 2>&1 &
qpid=$!
exec 3<> "$FIFO"

wait_pat() {
    local pat=$1 tmo=$2 i=0
    local lim=$((tmo * 5))
    while [ "$i" -lt "$lim" ]; do
        grep -aq -- "$pat" "$OUT" 2>/dev/null && return 0
        kill -0 "$qpid" 2>/dev/null || return 1
        sleep 0.2
        i=$((i + 1))
    done
    return 1
}

wait_prompt_after() {
    local off=$1 tmo=$2 i=0
    local lim=$((tmo * 5))
    while [ "$i" -lt "$lim" ]; do
        tail -c +$((off + 1)) "$OUT" 2>/dev/null | grep -aq '[~/] # ' && return 0
        kill -0 "$qpid" 2>/dev/null || return 1
        sleep 0.2
        i=$((i + 1))
    done
    return 1
}

if ! wait_pat '[~/] # ' 120; then
    echo "宿主 shell 未就绪：$OUT"
    kill "$qpid" 2>/dev/null
    exec 3>&-
    rm -f "$FIFO"
    exit 1
fi

send_step() {
    local cmd=$1 tmo=$2
    local off
    off=$(wc -c < "$OUT" 2>/dev/null)
    case "$off" in ''|*[!0-9]*) off=0 ;; esac
    printf '%s\n' "$cmd" >&3
    wait_prompt_after "$off" "$tmo"
}

rc=0
send_step "/bin/task-completion broadcast ${COMP_ARGS:-8 5}" 120 || rc=1
send_step 'echo TASKCOMP_DONE' 30 || rc=1

kill "$qpid" 2>/dev/null
wait "$qpid" 2>/dev/null
exec 3>&-
rm -f "$FIFO"

echo "=== TASKCOMP $ARCH smp=$SMPV ==="
grep -a 'TASKCOMP ' "$OUT" | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' | tr -d '\r'
echo "---"
echo "日志：$OUT"
[ "$rc" = 0 ] || { echo "❌ 未等到完成标记（超时或挂死）"; exit 1; }
