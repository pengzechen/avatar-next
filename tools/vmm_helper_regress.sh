#!/bin/bash
# vmm_helper_regress.sh — helper 模式（run-net → 宿主 shell → /bin/vmm-run）启动回归
#
# 与 boot_regress.sh 的区别：那个管**直启模式 + SMP=1**；本脚本管
# **helper 模式 + 多核** —— 历史上最容易出问题的那条路：
#   - 内核 setup（VMXON / IA32_FEATURE_CONTROL / 宿主 MSR 表 / hgatp）跑在
#     /bin/vmm-run 所在的那颗核上，而 vCPU 任务钉在另一颗核上；
#   - Ctrl+T k 停掉 guest 之后要能**再次启动**（fd 池、调度器不变量）。
# 根因清单见 docs/bugfix/SMP_HELPER_MODE_BUGFIX.md 与
# docs/vmm/X86_GUEST_LINUX.md §9.10~§9.12。
#
# 用法：
#   tools/vmm_helper_regress.sh <x86_64|aarch64|riscv64> [轮数] [smp] [--restart]
# 产物：/tmp/helper_rep_<arch>/smp<N>_<i>.log
#   --restart：每轮「启动 → Ctrl+T k 停 → 再启动」，要求 guest 起来两次
#
# 判据是 guest 自己那行 uname（"Linux (none) 6.2.15"）—— 宿主日志里不会有。
#
# ⚠️ 输入**必须等宿主提示符出现之后再喂**：TCG 架构（aarch64/riscv64）在机器
#    有负载时宿主启动可能远超固定 sleep，命令会被丢在 tty 缓冲里或压根没到
#    shell —— 实测会误报成「guest 起不来」。所以这里用 FIFO 挂着 stdin、
#    轮询日志里的提示符，而不是 `{ sleep N; printf ...; } |`。
#
# ⚠️ **别并行跑这个脚本、也别在构建期间跑**：vCPU 任务钉在单核上，机器被压满
#    时它会抢不到 CPU —— guest 自己的时钟在 90 秒墙上时间里只走 0.5 秒，
#    看起来像「SMP=4 起不来」，其实空载时 2 秒就起来了（2026-09-27 实测踩过）。
#    要并行验证多个架构，请串行跑。
set -u
cd "$(dirname "$0")/.." || exit 1

# 同一时刻只允许一个实例：这个脚本靠"vCPU 能拿到 CPU"来判定成败，两个实例
# （或一个实例 + 一场构建）并行跑会互相把对方饿死，结果全是假失败。
LOCK=/tmp/.vmm_helper_regress.lock
exec 9>"$LOCK"
if command -v flock >/dev/null 2>&1 && ! flock -n 9; then
    echo "已有另一个 vmm_helper_regress 在跑（或机器上有别的重负载）—— 等它结束再来"
    exit 1
fi

ARCH=${1:-x86_64}; N=${2:-6}; SMPV=${3:-2}; RESTART=${4:-}
case "$ARCH" in
  x86_64)  Q="qemu-system-x86_64 -machine microvm -enable-kvm -cpu host"
           K=build/qemu-virt-x86_64/kernel_x86_64.bin
           R=build/qemu-virt-x86_64/rootfs-x86_64.img; RA=0x4000000 ;;
  aarch64) Q="qemu-system-aarch64 -cpu cortex-a76 -M virt,virtualization=on,gic-version=3"
           K=build/qemu-virt-aarch64/kernel_aarch64.bin
           R=build/qemu-virt-aarch64/rootfs-aarch64.img; RA=0x5fe00000 ;;
  riscv64) Q="qemu-system-riscv64 -M virt -bios default"
           K=build/qemu-virt-riscv64/kernel_riscv64.bin
           R=build/qemu-virt-riscv64/rootfs-riscv64.img; RA=0x88000000 ;;
  *) echo "用法: $0 <x86_64|aarch64|riscv64> [轮数] [smp] [--restart]"; exit 1 ;;
esac
[ -f "$K" ] || { echo "缺 $K —— 先 make PLATFORM=qemu-virt-$ARCH SMP=$SMPV kernel rootfs"; exit 1; }
[ -f "$R" ] || { echo "缺 $R —— 先 make PLATFORM=qemu-virt-$ARCH SMP=$SMPV kernel rootfs"; exit 1; }

# 变体自检：本脚本测的是 **helper 模式**，内核必须是**非** GUEST_LINUX 变体。
# 直启变体会在启动时自己引导 guest，而 guest 的提示符也是 `~ #` —— 脚本会把它
# 当成宿主就绪、把 /bin/vmm-run 喂进 guest（回显 "not found"），于是把
# 「直启成功」误判成「helper 成功」，Ctrl+T k 那轮则必然失败。
# 2026-09-27 实测踩过：跑直启门禁（GUEST_LINUX=1 SMP=1）会**覆盖 build/ 里的
# 内核**，之后拿它跑 SMP=4 回归，整轮数据作废（见 CLAUDE.md 变体标志那一条）。
if strings "$K" 2>/dev/null | grep -q 'GUEST_LINUX mode'; then
    echo "❌ $K 是 GUEST_LINUX 直启变体，不能用来测 helper 模式。"
    echo "   重建（三行都要，顺序不能反）："
    echo "     make PLATFORM=qemu-virt-$ARCH SMP=$SMPV clean"
    echo "     make PLATFORM=qemu-virt-$ARCH SMP=$SMPV kernel rootfs"
    exit 1
fi

OUT=/tmp/helper_rep_$ARCH; mkdir -p "$OUT"
FIFO=$OUT/.stdin.$$
# 各阶段等待窗口（秒）。机器越忙 guest 越慢 —— 窗口太短会把"慢"判成"起不来"，
# 所以宁可放宽（空载时 2 秒就起来，这些窗口正常情况下根本用不满）。
PROMPT_WAIT=${PROMPT_WAIT:-120}
GUEST_WAIT=${GUEST_WAIT:-180}
STOP_WAIT=${STOP_WAIT:-30}
GST="Linux (none) 6.2.15"          # guest 的 uname（宿主不会有）
HOSTPROMPT='[~/] # $'              # 宿主 busybox 提示符（可能没有换行）
# shellcheck disable=SC2086
QF="$Q -smp $SMPV -m 2G -nographic -kernel $K -device loader,file=$R,addr=$RA,force-raw=on \
    -netdev user,id=net0 -device virtio-net-device,netdev=net0,mac=52:54:00:12:34:56"

# wait_count <file> <模式> <次数> <超时秒> [F]
#   F = 按**固定串**匹配（grep -F）。判据 "Linux (none) 6.2.15" 里有括号，
#   用 -E 时 "(none)" 会被当成**分组**、永远匹配不上 —— 实测把所有成功的轮次
#   都判成了「guest 启动 0 次」，白查了半天。带括号/正则元字符的判据一律传 F。
wait_count() {
    local f=$1 pat=$2 want=$3 tmo=$4 mode=${5:-E} i=0 lim n=0
    lim=$((tmo * 5))          # 单独一行：`set -u` 下同一行 local 里引用前一个
                              # 变量会被判 unbound（bash 先展开整条命令的词）

    while [ "$i" -lt "$lim" ]; do
        # grep -c 本来就会打印 0，别再补 `|| echo 0`（会变成两行，
        # `[ "0\n0" -ge 1 ]` 直接报 integer expression expected）。
        # 文件还没建出来时 grep 什么都不打印，这里统一归一成 0。
        n=$(grep -ac$mode -- "$pat" "$f" 2>/dev/null)
        case "$n" in ''|*[!0-9]*) n=0 ;; esac
        [ "$n" -ge "$want" ] && return 0
        sleep 0.2; i=$((i + 1))
    done
    return 1
}

# wait_count_from <file> <起始字节偏移> <模式> <次数> <超时秒> [F]
#   只看 offset **之后**新增的内容 —— 用来判断"Ctrl+T k 之后宿主提示符**重新**
#   出现"，而不是复用之前那次启动时的提示符。
wait_count_from() {
    local f=$1 off=$2 pat=$3 want=$4 tmo=$5 mode=${6:-E}
    local i=0 lim n=0
    lim=$((tmo * 5))
    while [ "$i" -lt "$lim" ]; do
        n=$(tail -c +$((off + 1)) "$f" 2>/dev/null | grep -ac$mode -- "$pat")
        case "$n" in ''|*[!0-9]*) n=0 ;; esac
        [ "$n" -ge "$want" ] && return 0
        sleep 0.2; i=$((i + 1))
    done
    return 1
}

pass=0; fail=0; unreal=0
for i in $(seq 1 "$N"); do
    log=$OUT/smp${SMPV}_$i.log
    rm -f "$log" "$FIFO"; mkfifo "$FIFO"
    t0=$SECONDS
    # shellcheck disable=SC2086
    timeout $((PROMPT_WAIT + 2 * GUEST_WAIT + STOP_WAIT + 60)) $QF < "$FIFO" > "$log" 2>&1 &
    qpid=$!
    exec 3<> "$FIFO"        # 读写同时打开：即使 QEMU 先退也不会卡在 open 上

    if ! wait_count "$log" "$HOSTPROMPT" 1 "$PROMPT_WAIT"; then
        # 宿主 shell 都没等到 —— 这是**测试环境问题**，不算 guest 失败
        unreal=$((unreal + 1))
        echo "  #$i 宿主 shell 未就绪（不作数，建议降载重跑）"
    else
        printf '/bin/vmm-run\n' >&3
        if [ "$RESTART" = "--restart" ]; then
            wait_count "$log" "$GST" 1 "$GUEST_WAIT" F
            off=$(wc -c < "$log" 2>/dev/null); case "$off" in ''|*[!0-9]*) off=0 ;; esac
            printf '\x14k' >&3                             # Ctrl+T k
            # 等 guest 真的收尾再启第二次：没停就再敲 vmm-run 只会**接入**
            # 还在跑的 guest（attach），不会产生第二个 uname → 假失败。
            #
            # 只等 "stop requested" 是不够的 —— 那个标记出现时 helper 还没退出、
            # 宿主 shell 也还没回到读 stdin，紧接着敲的 vmm-run 会掉在缝里：
            # riscv64 SMP=2 实测 2/4 假失败，每轮白等满 180s（明明 stop 流程
            # 与 `vcpu0 exited normally` 都正常）。所以还要等宿主提示符在
            # offset **之后**重新出现（= shell 回来了）再喂。
            wait_count_from "$log" "$off" 'stop requested|guest stopping' 1 "$STOP_WAIT"
            wait_count_from "$log" "$off" "$HOSTPROMPT" 1 "$STOP_WAIT" || sleep 2
            printf '/bin/vmm-run\n' >&3
            wait_count "$log" "$GST" 2 "$GUEST_WAIT" F
            n=$(grep -acF -- "$GST" "$log" 2>/dev/null); case "$n" in ''|*[!0-9]*) n=0 ;; esac
        else
            wait_count "$log" "$GST" 1 "$GUEST_WAIT" F
            n=$(grep -acF -- "$GST" "$log" 2>/dev/null); case "$n" in ''|*[!0-9]*) n=0 ;; esac
        fi
        want=1; [ "$RESTART" = "--restart" ] && want=2
        if [ "$n" -ge "$want" ]; then pass=$((pass+1))
        else fail=$((fail+1)); echo "  #$i FAIL：guest 启动 $n 次（要 $want 次）"; fi
    fi

    exec 3>&-; kill "$qpid" 2>/dev/null; wait "$qpid" 2>/dev/null
    printf "\r  %s smp=%s %d/%d 通过=%d 失败=%d 未就绪=%d（本轮 %ds）" \
           "$ARCH" "$SMPV" "$i" "$N" "$pass" "$fail" "$unreal" "$((SECONDS - t0))"
done
rm -f "$FIFO"
echo
echo "=== $ARCH SMP=$SMPV $([ "$RESTART" = "--restart" ] && echo '(含重启)' || echo ''): 通过 $pass / 失败 $fail / 未就绪 $unreal ==="
echo "日志：$OUT"
[ "$fail" = 0 ] && [ "$unreal" = 0 ]
