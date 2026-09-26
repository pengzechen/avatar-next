#!/bin/bash
# x86_64 guest Linux —— 构建 / 启动 / 进度摘要
#
# 用法:
#   bash tools/x86-guest-boot.sh build     # 编 rootfs + 内核（同变体，顺序已修正）
#   bash tools/x86-guest-boot.sh run       # 启动 VMM，跑 60 秒并把 guest 输出摘出来
#   bash tools/x86-guest-boot.sh native    # 裸跑同一镜像（能进 ~ # shell 的基线对照）
#   bash tools/x86-guest-boot.sh all       # build + run
#
set -e
cd "$(dirname "$0")/.."
P=qemu-virt-x86_64
V=GUEST_LINUX=1
IMG=imgs/guests/x86_64
LIB=build/$P

build() {
  # ⚠️ rootfs 会连带重编内核，用的却是这条命令里的变体 ——
  #    所以两个目标必须同变体，且 kernel 放最后，否则内核被悄悄换成非 guest 版
  #    （症状：guest 一个字符都不输出，宿主 busybox 直接跑起来）。
  make PLATFORM=$P $V rootfs
  make PLATFORM=$P $V kernel
  echo -n "GUEST_LINUX 校验(应为 1): "
  strings $LIB/kernel_x86_64.bin | grep -c 'GUEST_LINUX mode'
}

run() {
  # 必须 -enable-kvm -cpu host（TCG 不模拟 VMX）；
  # 用 -display none -serial stdio，不要用 -nographic（x86 上不出输出）。
  local log=/tmp/x86_guest_run.log
  timeout "${1:-60}" qemu-system-x86_64 -machine q35 -enable-kvm -cpu host \
    -smp 1 -m 2G -display none -serial stdio \
    -kernel $LIB/kernel_x86_64.bin \
    -device loader,file=$LIB/rootfs-x86_64.img,addr=0x4000000,force-raw=on \
    < /dev/null > "$log" 2>&1 || true
  echo "=== guest 侧输出（去掉宿主 klog）==="
  sed 's/\x1b\[[0-9;]*m//g' "$log" | grep -vE '^\[(INFO|WARN|ERROR)\]' | grep -v terminating | head -30
  echo "=== 进度摘要 ==="
  # 判据是 guest init 里那行 `uname -a`（hostname 是 (none)，宿主日志里不会有）
  echo -n "到达 shell:      "; grep -ac 'Linux (none)' "$log" || true
  echo -n "guest 写串口次数: "; grep -ac 'reason=30' "$log" || true
  echo -n "最近一次采样:     "; sed 's/\x1b\[[0-9;]*m//g' "$log" | grep -a RIP-SAMPLE | tail -1 || true
  echo "完整日志: $log"
}

native() {
  timeout 40 qemu-system-x86_64 -enable-kvm -cpu host -m 1G -display none -serial stdio \
    -kernel $IMG/bzImage -initrd $IMG/initrd.gz \
    -append "console=ttyS0 earlycon=uart8250,io,0x3f8 rdinit=/init nox2apic \
             no_timer_check tsc=unstable irqpoll pci=conf1 pci=nomsi acpi=off" \
    < /dev/null 2>&1 | tail -20
}

case "${1:-all}" in
  build)  build ;;
  run)    run "${2:-60}" ;;
  native) native ;;
  all)    build; run 60 ;;
  *)      echo "用法: $0 [build|run|native|all]"; exit 1 ;;
esac
