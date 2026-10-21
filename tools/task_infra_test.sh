#!/bin/bash
# task_infra_test.sh — 用户态验收 task 基础设施（多核分发 / 跨核唤醒）
#
# 跑的是 apps/c/task_ipc_test.c（rootfs 里的 /bin/task-ipc）：
#   dist    : fork N 个进程，看它们落在哪些 CPU
#   ping    : pipe 乒乓，量化跨核唤醒延迟（核心判据）
#
# 同一份测试程序可以直接跑在「新内核」和「基线内核」上，用输出做 A/B：
#   - 新内核：远端入队/唤醒会发 resched IPI，延迟应是几十~几百 us；
#   - 基线：  没有 IPI，被唤醒的任务要等目标核下一次 tick（100Hz = 10ms）。
#
# 用法：
#   tools/task_infra_test.sh <arch> [smp]
# 环境变量：
#   KERNEL_BIN / ROOTFS_IMG 指向要测的内核与 rootfs（默认 build/ 下的当前产物）
#   OUT                      日志路径（默认 /tmp/task_infra_<arch>.log）
set -u
cd "$(dirname "$0")/.." || exit 1

ARCH=${1:-x86_64}
SMPV=${2:-2}
OUT=${OUT:-/tmp/task_infra_$ARCH.log}

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
if strings "$K" 2>/dev/null | grep -q 'GUEST_LINUX mode'; then
    echo "❌ $K 是 GUEST_LINUX 直启变体，测的是 guest 不是宿主 shell"
    exit 1
fi

FIFO=/tmp/.task_infra_$$.fifo
rm -f "$FIFO" "$OUT"
mkfifo "$FIFO"

# shellcheck disable=SC2086
timeout 420 $Q -smp "$SMPV" -m 2G -nographic -kernel "$K" \
    -device loader,file="$R",addr="$RA",force-raw=on \
    < "$FIFO" > "$OUT" 2>&1 &
qpid=$!
exec 3<> "$FIFO"

wait_pat() { # <pattern> <timeout-sec>
    # `set -u` 下同一行 local 里引用前一个变量会被判 unbound（bash 先展开
    # 整条命令的词）—— 必须拆行，见 vmm_helper_regress.sh 的同类注释。
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

if ! wait_pat '[~/] # ' 120; then
    echo "宿主 shell 未就绪（机器忙？）：$OUT"
    kill "$qpid" 2>/dev/null
    exec 3>&-
    rm -f "$FIFO"
    exit 1
fi

# 等 offset 之后**新出现**一个宿主提示符 = 上一条命令执行完了。
# 不能一次性把三条命令灌进串口：模拟 16550 的 FIFO 很小，guest 忙的时候
# 会丢字符（实测第三条只收到 "/bin/task-ipc fan"）。
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

send_step() { # <command> <timeout-sec>
    local cmd=$1 tmo=$2
    local off
    off=$(wc -c < "$OUT" 2>/dev/null)
    case "$off" in ''|*[!0-9]*) off=0 ;; esac
    printf '%s\n' "$cmd" >&3
    wait_prompt_after "$off" "$tmo"
}

rc=0
send_step '/bin/task-ipc dist 8' 60 || rc=1
send_step "/bin/task-ipc ping ${PING_ARGS:-100 3}" 180 || rc=1
send_step 'echo TASKINFRA_DONE' 30 || rc=1
kill "$qpid" 2>/dev/null
wait "$qpid" 2>/dev/null
exec 3>&-
rm -f "$FIFO"

echo "=== TASKIPC $ARCH smp=$SMPV ==="
grep -a 'TASKIPC ' "$OUT" | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' | tr -d '\r'
echo "---"
echo "日志：$OUT"
[ "$rc" = 0 ] || { echo "❌ 未等到完成标记（超时或挂死）"; exit 1; }
