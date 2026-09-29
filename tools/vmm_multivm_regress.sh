#!/bin/bash
# vmm_multivm_regress.sh — 同内核**多 VM** 回归（helper 模式）
#
# 验证的是多 VM 那条链路，而不是单 VM 能不能起来（那个归 vmm_helper_regress.sh）：
#   1. vmm-run            → vm1 跑起 guest Linux（自己 uname）
#   2. Ctrl+T d（0x14 0x64）→ detach：guest 留在后台，helper 退出，回到宿主 shell
#   3. vmm-run -n         → **新建** vm2（不是接入 vm1）
#   4. 两个 guest 都还活着：两行 uname、两个 vcpu0 任务
#   5b. `vmm-run -l` 连查两次，两次都必须报 2 个 RUNNING —— 纯查询不能有副作用
#   5c. 裸 `vmm-run` 接回**上次离开的那个**（vm2），而不是最小 vmid 的 vm1
#   6.  再 detach，vmm-run -a <vm1> 附着回**先启动的那个**，往串口敲一句看有没有回显
#   6b. 控制台命令：Ctrl+T l 列出 → 前台标记在第 1 行；Ctrl+T 2 切过去 → 标记到第 2 行
#
# 第 5 步看着多余，其实是多 VM 专属的一类回归的**唯一**探针：单 VM 永远测不出
# 「两颗 vCPU 共用一颗核时，本核当前的 VMCS 属于谁」这类问题。历史上就栽过一次
# —— 症状是"能输入、没回显"（见下面第 7 步的注释）。
#
# ⚠️ 第 5 步必须**显式**用 `-a <vm1>`，不能图省事写光秃秃的 `vmm-run`。
#    默认接入的语义是"接回你上次离开的那个"（screen -r 那套），所以从 vm2
#    detach 之后敲 `vmm-run` 接回的是 **vm2** —— 而 vm2 正是"踩人的"那一方，
#    它的控制台即便在 bug 复现时也是好的。用它当探针等于把这条回归作废。
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
timeout 540 $Q -smp "$SMPV" -m 2G -nographic -kernel "$K" \
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

echo "3) detach vm1（Ctrl+T d = 0x14 0x64）..."
printf '\x14d' >&3
# ⚠️ 必须停顿：要等 helper 真的收尾退出、控制台还给宿主 shell，否则紧接着
# 敲的命令会被**还在跑的 helper** 读走转发给 guest。
# （从前还多一个理由：0x1b 有 ~40ms 的转义序列消歧窗口。换成前缀键之后
#   那个窗口没有了 —— 前缀之后的字节一定是命令，不可能是转义序列的一部分。）
sleep 1

echo "4) 启动 vm2（vmm-run -n = 强制新建）..."
printf 'vmm-run -n\n' >&3
wait_count "$GST" 2 220 F && ok "vm2 也跑到 uname" || bad "vm2 没起来"

echo "5) detach vm2..."
printf '\x14d' >&3
sleep 1

# ── 5b. 纯查询不能有副作用 ──────────────────────────────────────
#
# 回归：`vmm-run -l` 曾经会把**正在跑的 guest 停掉**。根因是 close() 的兜底
# 语义「停掉前台 VM」，而 /dev/vmm 是全局单例设备 —— 一个从没 BOOT 过的进程
# open 一下再 close，照样走到那个破坏性分支。修复是内核侧加了「会话 owner」
# 守卫（见 vmm_dev.c 的 g_vmm_owner_pid）。
#
# 探针用「连查两次、两次都必须是 2」：只查一次的话，就算它真的停了 VM，
# 那一下也可能还在 DYING 没落到 FREE，数字看着仍然对。
echo "5b) vmm-run -l 是纯查询（不该动任何 VM）..."
OFF=$(wc -c < "$LOG"); printf 'vmm-run -l\n' >&3
sleep 3
L1=$(tail -c +$((OFF + 1)) "$LOG" 2>/dev/null)
OFF=$(wc -c < "$LOG"); printf 'vmm-run -l\n' >&3
sleep 3
L2=$(tail -c +$((OFF + 1)) "$LOG" 2>/dev/null)
# 表头也要看：只数 "RUNNING" 的话，表头整行消失（`[vmm-run]  #   vmid  state`）
# 这条断言照样绿 —— 实测被这么骗过一次。
N1=$(printf '%s' "$L1" | grep -ac 'RUNNING')
N2=$(printf '%s' "$L2" | grep -ac 'RUNNING')
if ! printf '%s' "$L1" | grep -aq 'vmid  state'; then
    bad "-l 没打出表头（只有数据行？）"
    N1=0
fi
if [ "$N1" -eq 2 ] && [ "$N2" -eq 2 ]; then
    ok "两次 -l 都报 2 个 RUNNING（查询无副作用）"
else
    bad "-l 之后 VM 数变了（$N1 → $N2）—— 查询有副作用"
fi

# ── 5c. 裸 vmm-run = 接回"上次离开的那个" ───────────────────────
#
# 这是本次另一个行为变化，必须单独验：从前接入只挑**最小 vmid**，所以从 vm2
# 分离后再敲 vmm-run 会接到 vm1（g_vmm_fg_vmid 明明记着 vm2 却没人看它）。
#
# ⚠️ 判据必须是 banner 里的 vmid，不能只看"接上了没有" —— 接到 vm1 也算接上。
echo "5c) 裸 vmm-run 应接回上次离开的 vm2（不是最小 vmid 的 vm1）..."
VM2=$(grep -aoE '\(vm[0-9]+\)' "$LOG" | sed -n 2p | tr -dc '0-9')
[ -n "$VM2" ] || VM2=2
OFF=$(wc -c < "$LOG"); printf 'vmm-run\n' >&3
sleep 5
NEW=$(tail -c +$((OFF + 1)) "$LOG" 2>/dev/null)
if printf '%s' "$NEW" | grep -aq "attached (vm$VM2)"; then
    ok "裸 vmm-run 接回了 vm$VM2（上次离开的那个）"
else
    bad "裸 vmm-run 没接回 vm$VM2 —— 多半又退化成挑最小 vmid 了"
fi
printf '\x14d' >&3
sleep 1

# ── 6. 附着回 **先启动的那个** VM，并验证串口真的通 ──────────────
#
# ⚠️ 这一步是回归门禁里最容易被忽略、也最容易坏的一环。历史 bug：
# x86 的 vmx_inject_pending() 用 vmcs_write() 写 VM_ENTRY_INTR_INFO，而
# VMREAD/VMWRITE 操作的是**本核当前装载的** VMCS —— 中断注入发生在
# vmm_arch_enter_guest() 的 VMPTRLD **之前**，于是两颗 vCPU 任务共用一颗核
# 时，vm2 的中断信息被写进了 **vm1 的 VMCS**；而 vlapic_accept_interrupt()
# 已经把 ISR 位置在了 vm2 的 vLAPIC 上。结果那个向量（236 = LAPIC timer）
# 既没送达、也永远不会被 EOI —— ISR 位永久卡住，guest 拿不到 tick，
# tty 的 workqueue 不跑。**症状是"能输入、没回显"**：输入确实进了 guest
# （IO-APIC 的另一条向量是好的），但屏幕上什么都没发生。
# 单 VM 时当前 VMCS 恰好一直是它的，所以这条只有多 VM 才测得出来。
#
# ⚠️ 必须**显式** `-a <vm1>`，不能图省事写光秃秃的 `vmm-run`：
#    默认接入的语义是"接回你上次离开的那个"（screen -r 那套），所以从 vm2
#    detach 之后敲 `vmm-run` 接回的是 **vm2** —— 而 vm2 正是"踩人的"那一方，
#    它的控制台即便在 bug 复现时也是好的。用它当探针等于把这条回归作废。
#    vm1 的 vmid 从 helper 自己打的 banner（`guest started (vmN)`）里取，
#    不写死 1（vmid 会回绕，虽然这里是全新启动的 QEMU）。
VM1=$(grep -aoE '\(vm[0-9]+\)' "$LOG" | head -1 | tr -dc '0-9')
[ -n "$VM1" ] || VM1=1
echo "6) vmm-run -a $VM1（附着回**先启动的**那个 VM）..."
printf 'vmm-run -a %s\n' "$VM1" >&3
sleep 5
printf 'echo ATTACH_BACK_OK\n' >&3
wait_count 'ATTACH_BACK_OK' 1 30 F && ok "附着回老 VM 后串口可用" \
                                  || bad "附着回老 VM 后串口不通（能输入没回显？）"

# ── 6b. 控制台里的命令（前缀 + 单键）────────────────────────────
#
# 覆盖本次改动的门面：`Ctrl+T l` 列 VM、`Ctrl+T <n>` 切前台。
# 判据取的是**表里带 `*` 那一行的序号**，而不是"switched to vm2"那句话 ——
# 后者只能证明 ATTACH ioctl 返回了 0，证明不了前台真的换了。
# 带 `*` 的行长这样（第 1 列是序号，第 2 列才是 vmid）：
#           1     1  RUNNING     *
#           2     2  RUNNING
echo "6b) 控制台命令：Ctrl+T l 列出、Ctrl+T 2 切换..."
OFF=$(wc -c < "$LOG"); printf '\x14l' >&3
sleep 2
NEW=$(tail -c +$((OFF + 1)) "$LOG" 2>/dev/null)
FG_BEFORE=$(printf '%s' "$NEW" | grep -a '\*' | head -1 | awk '{print $1}')
[ "$FG_BEFORE" = "1" ] && ok "Ctrl+T l 列出 VM，前台在第 1 行（刚 -a 的是 vm1）" \
                       || bad "Ctrl+T l 前台标记不对（拿到 '$FG_BEFORE'，期望 1）"

OFF=$(wc -c < "$LOG"); printf '\x142' >&3
sleep 2
OFF=$(wc -c < "$LOG"); printf '\x14l' >&3
sleep 2
NEW=$(tail -c +$((OFF + 1)) "$LOG" 2>/dev/null)
FG_AFTER=$(printf '%s' "$NEW" | grep -a '\*' | head -1 | awk '{print $1}')
[ "$FG_AFTER" = "2" ] && ok "Ctrl+T 2 把前台切到了第 2 个 VM" \
                      || bad "Ctrl+T 2 没切换前台（拿到 '$FG_AFTER'，期望 2）"

echo "7) 两个 VM 各自的分配/销毁记录："
grep -aE 'vm[0-9]+ allocated|vm[0-9]+ freed|task created' "$LOG" \
    | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' | tr -d '\r' | tail -8 | sed 's/^/   /'

N_GST=$(grep -acF "$GST" "$LOG")
N_TASK=$(grep -ac 'vcpu0 task created' "$LOG")
echo "8) 终端上 uname 出现 $N_GST 次（要 >= 2）"
[ "$N_GST" -ge 2 ] && ok "两个 guest 都在跑" || bad "只有 $N_GST 个 guest"
echo "9) 宿主侧 vcpu0 任务数 $N_TASK（要 == 2）"
[ "$N_TASK" -eq 2 ] && ok "两个 vCPU 任务" || bad "vCPU 任务数 $N_TASK"

echo "== $ARCH 多 VM：通过 $PASS / 失败 $FAIL =="
echo "   日志：$LOG"
[ "$FAIL" -eq 0 ]
