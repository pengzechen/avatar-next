#!/bin/bash
# fat_regress.sh — QEMU 上的 FAT(/boot) 读写回归
#
# 前置：已用 ROOTFS_MBR=1 构建了 p1 FAT + p2 ext4 的镜像和 kernel。
# 用法：KERNEL_BIN=... ROOTFS_IMG=... ROOTFS_ADDR=... tools/fat_regress.sh <arch>
#   或直接 make PLATFORM=qemu-virt-x86_64 ROOTFS_MBR=1 LOG=info test-fat
#
# 判据：
#   - 启动日志里 FAT 卷挂载成功，root ext4 没有失败；
#   - /boot 能读种子文件；能创建/写/读/删文件；mkdir/cp/rm -rf 正常；
#   - /etc/passwd（ext4 root）仍然可读。
set -u
cd "$(dirname "$0")/.." || exit 1

ARCH=${1:-x86_64}
K=${KERNEL_BIN:-build/qemu-virt-${ARCH}/kernel_${ARCH}.bin}
R=${ROOTFS_IMG:-build/qemu-virt-${ARCH}/rootfs-mbr-${ARCH}.img}
RA=${ROOTFS_ADDR:-}

case "$ARCH" in
  x86_64)
    [ -n "$RA" ] || RA=0x4000000
    if [ -e /dev/kvm ]; then
        QEMU=(qemu-system-x86_64 -machine q35 -enable-kvm -cpu host)
    else
        QEMU=(qemu-system-x86_64 -machine q35 -accel tcg -cpu max)
    fi
    ;;
  aarch64)
    [ -n "$RA" ] || RA=0x5fe00000
    QEMU=(qemu-system-aarch64 -cpu cortex-a76 -M virt,virtualization=on,gic-version=3)
    ;;
  riscv64)
    [ -n "$RA" ] || RA=0x88000000
    QEMU=(qemu-system-riscv64 -M virt -bios default)
    ;;
  *)
    echo "unsupported ARCH=$ARCH (use x86_64/aarch64/riscv64)" >&2
    exit 2
    ;;
esac

[ -f "$K" ] || { echo "missing kernel: $K" >&2; exit 1; }
[ -f "$R" ] || { echo "missing rootfs: $R (build with ROOTFS_MBR=1)" >&2; exit 1; }
[ -n "$RA" ] || { echo "missing ROOTFS_ADDR" >&2; exit 1; }

OUT=/tmp/fat_regress_$ARCH
mkdir -p "$OUT"
LOG=$OUT/run.log
FIFO=$OUT/.stdin.$$
rm -f "$LOG" "$FIFO"
mkfifo "$FIFO"

QPID=
cleanup() {
    if [ -n "$QPID" ] && kill -0 "$QPID" 2>/dev/null; then
        kill "$QPID" 2>/dev/null
        wait "$QPID" 2>/dev/null
    fi
    rm -f "$FIFO"
}
trap cleanup EXIT

fail() {
    echo "❌ $*" >&2
    echo "--- 日志尾部 ($LOG) ---" >&2
    tail -80 "$LOG" >&2 2>/dev/null
    exit 1
}

echo "=== FAT regression: arch=$ARCH ==="
"${QEMU[@]}" -smp 1 -m 2G -nographic -kernel "$K" \
    -device loader,file="$R",addr="$RA",force-raw=on \
    < "$FIFO" > "$LOG" 2>&1 &
QPID=$!
exec 3>"$FIFO"   # 保持写端打开，避免 QEMU 收到 EOF

wait_for() {
    local pat=$1 tmo=$2 i
    for ((i = 0; i < tmo; i++)); do
        if grep -aq "$pat" "$LOG" 2>/dev/null; then
            return 0
        fi
        kill -0 "$QPID" 2>/dev/null || return 1
        sleep 1
    done
    return 1
}

# 交互 shell 的提示符可能是 `~ #`（HOME=/root）或 `/ #`（cwd=/）；
# 直接等 ` # `，两种都能命中。
wait_for ' # ' 120 || fail "kernel did not reach the shell prompt"
grep -aq 'Root filesystem mounted successfully' "$LOG" || fail "ext4 root was not mounted"
grep -aq 'FAT volume mounted at' "$LOG" || fail "FAT volume was not mounted"
grep -aqE 'ext4_mount failed|FAT mount at .* failed|panic' "$LOG" && fail "boot log contains a mount/panic error"

CMD_N=0
run_cmd() {
    local cmd=$1 tmo=${2:-60}
    CMD_N=$((CMD_N + 1))
    local start end marker
    start=$(stat -c %s "$LOG")
    marker="__FAT_DONE_${CMD_N}__"
    printf '%s\n' "$cmd" >&3
    printf 'echo %s\n' "$marker" >&3
    wait_for "$marker" "$tmo" || fail "command timed out: $cmd"
    end=$(stat -c %s "$LOG")
    tail -c +$((start + 1)) "$LOG" | head -c $((end - start)) > "$OUT/cmd_${CMD_N}.log"
}

seg() { cat "$OUT/cmd_${CMD_N}.log"; }
assert_has() {
    seg | grep -aq -- "$1" || fail "cmd #$CMD_N output missing: $1"
}
assert_not() {
    if seg | grep -aq -- "$1"; then
        fail "cmd #$CMD_N output unexpectedly contains: $1"
    fi
}

run_cmd 'ls /boot'
assert_has 'hello.txt'
assert_has 'boot.cmd'

# chdir + 相对路径也要走 FAT 挂载
run_cmd 'cd /boot'
run_cmd 'ls'
assert_has 'hello.txt'
run_cmd 'cd /'

run_cmd 'cat /boot/hello.txt'
assert_has 'hello from FAT'

run_cmd 'echo write-test > /boot/new.txt'
run_cmd 'cat /boot/new.txt'
assert_has 'write-test'

run_cmd 'echo second >> /boot/new.txt'
run_cmd 'cat /boot/new.txt'
assert_has 'write-test'
assert_has 'second'

run_cmd 'mv /boot/hello.txt /boot/hello2.txt'
run_cmd 'cat /boot/hello2.txt'
assert_has 'hello from FAT'
run_cmd 'mv /boot/hello2.txt /boot/hello.txt'

# 跨挂载 mv：renameat 必须返回 EXDEV，busybox 应该退化成 copy+unlink
run_cmd 'mv /boot/hello.txt /tmp/hello.txt'
run_cmd 'cat /tmp/hello.txt'
assert_has 'hello from FAT'
run_cmd 'mv /tmp/hello.txt /boot/hello.txt'
run_cmd 'cat /boot/hello.txt'
assert_has 'hello from FAT'

run_cmd 'mkdir /boot/sub'
run_cmd 'cp /etc/passwd /boot/sub/passwd'
run_cmd 'cat /boot/sub/passwd'
assert_has 'root::0:0:root'

run_cmd 'ls -l /boot'
assert_has 'new.txt'
assert_has 'sub'

run_cmd 'rm /boot/new.txt'
run_cmd 'rm -rf /boot/sub'

run_cmd 'ls /boot'
assert_has 'hello.txt'
assert_not 'new.txt'
assert_not 'sub'

run_cmd 'cat /etc/passwd'
assert_has 'root::0:0:root'

echo "=== FAT regression passed ($CMD_N commands) ==="
