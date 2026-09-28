#!/bin/bash
# boot_regress.sh — x86_64 guest 启动回归：连续 N 次都要进 shell
#
# 背景：guest 曾出现过**间歇性**卡死（约 1/3 概率），停在 init 早期不出来。
# 根因是 GS 这一对 MSR 的虚拟化（guest 的 swapgs 被 VM-entry 的 load 表覆盖），
# 见 kernel/vmm/x86_64/vmx.c 里 vmx_refresh_host_state()/vmx_msr_lists_init()
# 的注释。这个脚本就是那条修复的回归门禁。
#
# 用法： tools/boot_regress.sh [次数] [每次超时秒]
# 产物： /tmp/boot_regress/<n>.log（失败的那次会另外留 hb.log 心跳尾部）

set -u
cd "$(dirname "$0")/.."

N=${1:-500}
TMO=${2:-90}
OUT=${OUT:-/tmp/boot_regress}
KERNEL=build/qemu-virt-x86_64/kernel_x86_64.bin
ROOTFS=build/qemu-virt-x86_64/rootfs-x86_64.img

mkdir -p "$OUT"
[ -f "$KERNEL" ] && [ -f "$ROOTFS" ] || { echo "先构建：make PLATFORM=qemu-virt-x86_64 GUEST_LINUX=1 LOG=warn SMP=1 kernel rootfs"; exit 1; }

pass=0; fail=0
t0=$(date +%s)

for i in $(seq 1 "$N"); do
    log="$OUT/$i.log"

    # stdin 用 process substitution 保持打开（-serial stdio 会读它）
    qemu-system-x86_64 -machine q35 -enable-kvm -cpu host -smp 1 -m 2G \
        -display none -serial stdio \
        -kernel "$KERNEL" \
        -device loader,file="$ROOTFS",addr=0x4000000,force-raw=on \
        < <(sleep $((TMO + 30))) >"$log" 2>&1 &
    qpid=$!

    ok=0
    for _ in $(seq 1 "$TMO"); do
        if grep -aq '~ #' "$log" 2>/dev/null; then ok=1; break; fi
        kill -0 "$qpid" 2>/dev/null || break
        sleep 1
    done
    kill "$qpid" 2>/dev/null
    wait "$qpid" 2>/dev/null

    if [ "$ok" = 1 ]; then
        pass=$((pass + 1))
        rm -f "$log"                     # 成功的日志不留（500 次会占满盘）
    else
        fail=$((fail + 1))
        mv "$log" "$OUT/fail-$i.log" 2>/dev/null
        grep -a 'HB-DBG' "$OUT/fail-$i.log" 2>/dev/null | tail -20 > "$OUT/fail-$i.hb"
        echo "### 第 $i 次失败（累计 pass=$pass fail=$fail）—— 现场：$OUT/fail-$i.log"
    fi

    if [ $((i % 10)) = 0 ] || [ "$ok" = 0 ]; then
        el=$(( $(date +%s) - t0 ))
        echo "[$(date +%H:%M:%S)] $i/$N  pass=$pass fail=$fail  已用 ${el}s（平均 $((el / i))s/次）"
    fi

    [ "$fail" -gt 20 ] && { echo "失败过多，提前停止"; break; }
done

el=$(( $(date +%s) - t0 ))
echo "=== 结束：$pass/$((pass + fail)) 次进 shell，用时 ${el}s ==="
exit $([ "$fail" = 0 ] && echo 0 || echo 1)
