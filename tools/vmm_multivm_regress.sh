#!/bin/bash
# vmm_multivm_regress.sh — 同内核**多 VM** 回归（helper 模式）
#
# 验证的是多 VM 那条链路，而不是单 VM 能不能起来（那个归 vmm_helper_regress.sh）：
#   1. vmm-run            → vm1 跑起 guest Linux（自己 uname）
#   2. Ctrl+[（0x1b）     → detach：guest 留在后台，helper 退出，回到宿主 shell
#   3. vmm-run -n         → **新建** vm2（不是接入 vm1）
#   4. 两个 guest 都还活着：两行 uname、两个 vcpu0 任务
#
# 判据用 guest 自己的 uname（"Linux (none) 6.2.15"）—— 宿主日志里不会有。
#
# ⚠️ 变体自检：本脚本跑的是 **helper 模式**（宿主 shell → /bin/vmm-run）。
# 直启变体（GUEST_LINUX=1）会在启动时自己引导 guest，那个 "~ #" 是 guest 的
# shell，脚本会把命令全喂进 guest —— 完全测错东西。所以下面先查一次字符串。
#
# ⚠️ 别并行跑、也别在构建期间跑：vCPU 任务钉在单核上，机器被压满时 guest 在
# 90 秒墙上时间里只走 0.5 秒（见 vmm_helper_regress.sh 顶部那段）。
#
# 用法：tools/vmm_multivm_regress.sh <aarch64|riscv64|x86_64> [smp]
set -u
cd "$(dirname "$0")/.." || exit 1

ARCH=${1:-aarch64}; SMPV=${2:-1}
case "$ARCH" in
  aarch64) Q="qemu-system-aarch64 -cpu cortex-a76 -M virt,virtualization=on,gic-version=3"
           K=build/qemu-virt-aarch64/kernel_aarch64.bin
           R=build/qemu-virt-aarch64/rootfs-aarch64.img; RA=0x5fe00000 ;;
  riscv64) Q="qemu-system-riscv64 -M virt -bios default"
           K=build/qemu-virt-riscv64/kernel_riscv64.bin
           R=build/qemu-virt-riscv64/rootfs-riscv64.img; RA=0x88000000 ;;
  x86_64)  Q="qemu-system-x86_64 -machine q35 -enable-kvm -cpu host"
           K=build/qemu-virt-x86_64/kernel_x86_64.bin
           R=build/qemu-virt-x86_64/rootfs-x86_64.img; RA=0x4000000 ;;
  *) echo "用法: $0 <aarch64|riscv64|x86_64> [smp]"; exit 1 ;;
esac

[ -f "$K" ] || { echo "缺 $K"; exit 1; }
[ -f "$R" ] || { echo "缺 $R"; exit 1; }
if strings "$K" 2>/dev/null | grep -q 'GUEST_LINUX mode'; then
    echo "❌ $K 是 GUEST_LINUX 直启变体，不能用来测 helper 模式"
    echo "   重建：make PLATFORM=qemu-virt-$ARCH SMP=$SMPV clean && make PLATFORM=qemu-virt-$ARCH SMP=$SMPV kernel rootfs"
    exit 1
fi

OUT=/tmp/multivm_$ARCH; mkdir -p "$OUT"
LOG=$OUT/session.log
FIFO=$OUT/.stdin.$$
rm -f "$LOG" "$FIFO"; mkfifo "$FIFO"

# shellcheck disable=SC2086
timeout 420 $Q -smp "$SMPV" -m 2G -nographic -kernel "$K" \
    -device loader,file="$R",addr=$RA,force-raw=on \
    < "$FIFO" > "$LOG" 2>&1 &
QPID=$!
exec 3<> "$FIFO"          # 保持写端打开，否则 QEMU 会立刻看到 EOF

# wait_count <模式> <次数> <超时秒> [F]   返回非 0 表示超时
wait_count() {
    local pat=$1 want=$2 tmo=$3 mode=${4:-E} i=0 lim n=0
    lim=$((tmo * 5))
    while [ "$i" -lt "$lim" ]; do
        n=$(grep -ac$mode -- "$pat" "$LOG" 2>/dev/null)
        case "$n" in ''|*[!0-9]*) n=0 ;; esac
        [ "$n" -ge "$want" ] && return 0
        sleep 0.2; i=$((i + 1))
    done
    return 1
}

# 判据里有括号，必须用 -F（-E 下 "(none)" 是分组，永远匹配不上 —— 踩过）
GST='Linux (none) 6.2.15'
HOST='Launching busybox shell'
PASS=0; FAIL=0
ok()   { echo "   ✓ $*"; PASS=$((PASS + 1)); }
bad()  { echo "   ❌ $*"; FAIL=$((FAIL + 1)); }
cleanup() { exec 3>&-; kill $QPID 2>/dev/null; wait $QPID 2>/dev/null; }
trap cleanup EXIT

echo "== $ARCH 多 VM 回归（SMP=$SMPV）=="

echo "1) 等宿主 shell..."
wait_count "$HOST" 1 180 F && ok "宿主就绪" || { bad "宿主没起来"; exit 1; }

echo "2) 启动 vm1..."
printf 'vmm-run\n' >&3
wait_count "$GST" 1 180 F && ok "vm1 跑到 uname" || bad "vm1 没起来"

echo "3) detach vm1（Ctrl+[ = 0x1b）..."
printf '\033' >&3
# ⚠️ 必须停顿：helper 的 esc_is_standalone() 有 ~40ms 消歧窗口，收到 0x1b 后
# 要等一下看有没有后续字节（方向键那种转义序列）。紧接着发命令的话，0x1b
# 会被当成转义序列的开头转发给 guest，detach 不发生。
sleep 1

echo "4) 启动 vm2（vmm-run -n = 强制新建）..."
printf 'vmm-run -n\n' >&3
wait_count "$GST" 2 220 F && ok "vm2 也跑到 uname" || bad "vm2 没起来"

echo "5) 两个 VM 各自的分配/销毁记录："
grep -aE 'vm[0-9]+ allocated|vm[0-9]+ freed|task created' "$LOG" \
    | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' | tr -d '\r' | tail -8 | sed 's/^/   /'

N_GST=$(grep -acF "$GST" "$LOG")
N_TASK=$(grep -ac 'vcpu0 task created' "$LOG")
echo "6) 终端上 uname 出现 $N_GST 次（要 >= 2）"
[ "$N_GST" -ge 2 ] && ok "两个 guest 都在跑" || bad "只有 $N_GST 个 guest"
echo "7) 宿主侧 vcpu0 任务数 $N_TASK（要 == 2）"
[ "$N_TASK" -eq 2 ] && ok "两个 vCPU 任务" || bad "vCPU 任务数 $N_TASK"

echo "== $ARCH 多 VM：通过 $PASS / 失败 $FAIL =="
echo "   日志：$LOG"
[ "$FAIL" -eq 0 ]
