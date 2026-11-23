#!/bin/sh
# SPDX-License-Identifier: MIT
#
# tools/make_rootfs.sh — 从 staging 目录生成 rootfs 镜像
#
# 由顶层 Makefile 的 $(ROOTFS_IMG) 规则调用。所有配置都通过环境变量传入
# （变量名与 Makefile 保持一致），镜像路径作为第一个参数。把这段逻辑放在
# 脚本里而不是 Makefile recipe 里，是为了让顶层 Makefile 只保留依赖关系。
#
# 依赖：mkfs.ext4（e2fsprogs >= 1.43，支持 -d）；ROOTFS_MBR=1 时还要
#       tools/make_mbr_image.sh。

set -eu

ROOTFS_IMG=${1:?usage: make_rootfs.sh <rootfs-img>}
: "${ARCH:?}" "${ROOTFS_STAGE:?}" "${ROOTFS_SIZE_MB:?}" "${ROOTFS_MBR:?}"
: "${FAT_PART_MB:?}" "${FAT_MOUNT_POINT:?}" "${BUILD_DIR:?}" "${LTP_BIN_DIR:?}"

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# install_bin <源文件> <目标名>：空变量/文件不存在时跳过。
install_bin()
{
    src=$1
    name=$2

    [ -n "$src" ] || return 0
    [ -f "$src" ] || return 0
    cp "$src" "$ROOTFS_STAGE/bin/$name"
    chmod +x "$ROOTFS_STAGE/bin/$name"
    echo "  [$name installed → /bin/$name]"
}

echo "=== Building rootfs for $ARCH: $ROOTFS_IMG ==="
rm -rf "$ROOTFS_STAGE"
mkdir -p "$ROOTFS_STAGE/bin"
# 虚拟文件系统挂载点（pseudofs 在内核侧拦截，ext4 只需目录项存在）
mkdir -p "$ROOTFS_STAGE/dev" "$ROOTFS_STAGE/proc" "$ROOTFS_STAGE/sys"
# FAT 挂载点（默认 /boot）在 ext4 里也要有目录项，这样 `ls /` 看得到
mkdir -p "$ROOTFS_STAGE$FAT_MOUNT_POINT"
mkdir -p "$ROOTFS_STAGE/root" "$ROOTFS_STAGE/tmp" "$ROOTFS_STAGE/etc"

# 安装 busybox
if [ -f "apps/busybox-$ARCH" ]; then
    cp "apps/busybox-$ARCH" "$ROOTFS_STAGE/busybox"
    chmod +x "$ROOTFS_STAGE/busybox"
    for applet in sh ls cat echo pwd mkdir rm cp mv grep find ps kill dd time; do
        cp "apps/busybox-$ARCH" "$ROOTFS_STAGE/bin/$applet"
        chmod +x "$ROOTFS_STAGE/bin/$applet"
    done
    echo "  [busybox + applets installed]"
fi

# 安装所有 .bin.elf 优先于裸 .bin
installed=0
for elf in "$BUILD_DIR"/*.bin.elf; do
    [ -f "$elf" ] || continue
    name=$(basename "$elf" .bin.elf)
    cp "$elf" "$ROOTFS_STAGE/$name"
    chmod +x "$ROOTFS_STAGE/$name"
    echo "  [$name installed (bin.elf)]"
    installed=$((installed + 1))
done
for elf in "$BUILD_DIR"/*.elf; do
    [ -f "$elf" ] || continue
    case "$elf" in
    *.bin.elf | */kernel_*.elf) continue ;;
    esac
    name=$(basename "$elf" .elf)
    cp "$elf" "$ROOTFS_STAGE/$name"
    chmod +x "$ROOTFS_STAGE/$name"
    echo "  [$name installed (elf)]"
    installed=$((installed + 1))
done
if [ "$installed" -eq 0 ]; then
    for bin in "$BUILD_DIR"/*.bin; do
        [ -f "$bin" ] || continue
        name=$(basename "$bin" .bin)
        cp "$bin" "$ROOTFS_STAGE/$name"
        chmod +x "$ROOTFS_STAGE/$name"
        echo "  [$name installed (bin)]"
    done
fi

# 安装用户态程序（变量为空或文件不存在时自动跳过）
install_bin "${VMM_RUN_BIN:-}" vmm-run
install_bin "${EPOLL_PERF_BIN:-}" epoll_perf
install_bin "${RACETEST_BIN:-}" racetest
install_bin "${RTPS_BIN:-}" rtps
install_bin "${RTCTL_BIN:-}" rtctl
install_bin "${RTEVENT_BIN:-}" rt-event-test
install_bin "${RTDEMO_USER_BIN:-}" rt-demo
install_bin "${TASKIPC_BIN:-}" task-ipc
install_bin "${TASKCOMP_BIN:-}" task-completion
install_bin "${DMATEST_BIN:-}" dmatest
install_bin "${DRMTEST_BIN:-}" drmtest

# 安装 nginx（如果存在对应架构的静态 musl 构建）
if [ -n "${NGINX_BIN:-}" ] && [ -f "$NGINX_BIN" ]; then
    mkdir -p "$ROOTFS_STAGE/bin" "$ROOTFS_STAGE/etc/nginx" "$ROOTFS_STAGE/www"
    cp "$NGINX_BIN" "$ROOTFS_STAGE/bin/nginx"
    chmod +x "$ROOTFS_STAGE/bin/nginx"
    printf '%s\n' \
        'worker_processes  1;' \
        'error_log /tmp/nginx-error.log info;' \
        'pid /tmp/nginx.pid;' \
        'events { worker_connections 64; }' \
        'http {' \
        '    access_log off;' \
        '    server {' \
        '        listen 80;' \
        '        root /www;' \
        '        location / { index index.html; }' \
        '    }' \
        '}' >"$ROOTFS_STAGE/etc/nginx/nginx.conf"
    printf '%s\n' '<html><body><h1>Avatar nginx</h1></body></html>' >"$ROOTFS_STAGE/www/index.html"
    echo "  [nginx installed → /bin/nginx]"
fi

# 安装 guest Linux 镜像（供 GUEST_LINUX 直启 / vmm-run 从 rootfs 加载）
# 目录名与 include/vmm/guest_loader.h 的 GUEST_LINUX_*_PATH 必须一致。
if [ "$ARCH" = "riscv64" ]; then
    missing=0
    for f in imgs/guests/rv64/linux.bin imgs/guests/rv64/linux.dtb \
        ${RISCV_GUEST_SMP_DTBS:-} imgs/guests/rv64/initrd.gz; do
        if [ ! -f "$f" ]; then
            echo "ERROR: missing guest image $f"
            missing=1
        fi
    done
    if [ "$missing" -ne 0 ]; then exit 1; fi
    mkdir -p "$ROOTFS_STAGE/guests/rv64"
    cp imgs/guests/rv64/linux.bin "$ROOTFS_STAGE/guests/rv64/linux.bin"
    cp imgs/guests/rv64/linux.dtb "$ROOTFS_STAGE/guests/rv64/linux.dtb"
    if [ -n "${RISCV_GUEST_SMP_DTBS:-}" ]; then
        cp ${RISCV_GUEST_SMP_DTBS} "$ROOTFS_STAGE/guests/rv64/"
    fi
    cp imgs/guests/rv64/initrd.gz "$ROOTFS_STAGE/guests/rv64/initrd.gz"
    echo "  [RISC-V Linux guest installed → /guests/rv64 (1cpu + smp DTBs)]"
fi

if [ "$ARCH" = "x86_64" ]; then
    missing=0
    for f in imgs/guests/x86_64/bzImage imgs/guests/x86_64/initrd.gz; do
        if [ ! -f "$f" ]; then
            echo "ERROR: missing guest image $f"
            missing=1
        fi
    done
    if [ "$missing" -ne 0 ]; then exit 1; fi
    mkdir -p "$ROOTFS_STAGE/guests/x86_64"
    cp imgs/guests/x86_64/bzImage "$ROOTFS_STAGE/guests/x86_64/bzImage"
    cp imgs/guests/x86_64/initrd.gz "$ROOTFS_STAGE/guests/x86_64/initrd"
    echo "  [x86_64 Linux guest installed → /guests/x86_64]"
fi

if [ "$ARCH" = "aarch64" ]; then
    missing=0
    for f in imgs/guests/aarch64/linux.bin ${GUEST_LINUX_DTB_SRC:-} \
        ${GUEST_LINUX_BASE_DTB_SRC:-} ${GUEST_LINUX_MULTI_DTBS:-} \
        imgs/guests/aarch64/initrd.gz; do
        if [ ! -f "$f" ]; then
            echo "ERROR: missing guest image $f"
            missing=1
        fi
    done
    if [ "$missing" -ne 0 ]; then exit 1; fi
    mkdir -p "$ROOTFS_STAGE/guests/linux"
    cp imgs/guests/aarch64/linux.bin "$ROOTFS_STAGE/guests/linux/linux.bin"
    cp ${GUEST_LINUX_DTB_SRC} "$ROOTFS_STAGE/guests/linux/linux.dtb"
    cp ${GUEST_LINUX_BASE_DTB_SRC} "$ROOTFS_STAGE/guests/linux/linux-1cpu.dtb"
    if [ -n "${GUEST_LINUX_MULTI_DTBS:-}" ]; then
        cp ${GUEST_LINUX_MULTI_DTBS} "$ROOTFS_STAGE/guests/linux/"
    fi
    cp imgs/guests/aarch64/initrd.gz "$ROOTFS_STAGE/guests/linux/initrd.gz"
    echo "  [AArch64 Linux guest installed → /guests/linux (${GUEST_LINUX_DTB_SRC}; runtime: 1cpu + smp DTBs)]"
fi

# 安装 Dropbear SSH 服务器
DROPBEAR_MULTI="third_party/dropbear-2024.86/dropbearmulti-$ARCH"
DROPBEAR_STRIP="$ARCH-linux-musl-strip"
if [ "$ARCH" = "riscv64" ]; then
    DROPBEAR_MULTI=third_party/dropbear-2024.86/dropbearmulti
    DROPBEAR_STRIP=riscv64-linux-musl-strip
fi
if [ -f "$DROPBEAR_MULTI" ]; then
    mkdir -p "$ROOTFS_STAGE/usr/sbin" "$ROOTFS_STAGE/usr/bin" "$ROOTFS_STAGE/etc/dropbear"
    "$DROPBEAR_STRIP" -o "$ROOTFS_STAGE/usr/sbin/dropbearmulti" "$DROPBEAR_MULTI"
    chmod +x "$ROOTFS_STAGE/usr/sbin/dropbearmulti"
    ln -sf dropbearmulti "$ROOTFS_STAGE/usr/sbin/dropbear"
    ln -sf ../sbin/dropbearmulti "$ROOTFS_STAGE/usr/bin/dropbearkey"
    echo "  [dropbear SSH installed]"
fi

# 安装 LTP 测例（如果已编译）
if [ -d "$LTP_BIN_DIR" ] &&
    [ "$(ls -1 "$LTP_BIN_DIR"/*.sh "$LTP_BIN_DIR"/[a-z]* 2>/dev/null | wc -l)" -gt 1 ]; then
    mkdir -p "$ROOTFS_STAGE/ltp"
    cp "$LTP_BIN_DIR"/* "$ROOTFS_STAGE/ltp/"
    chmod +x "$ROOTFS_STAGE/ltp/"*
    echo "  [LTP testcases installed → /ltp/]"
fi

# /etc/passwd: root 无密码；nobody 供 LTP getpwnam("nobody") 使用
printf 'root::0:0:root:/root:/bin/sh\nnobody:x:65534:65534:nobody:/tmp:/bin/sh\n' >"$ROOTFS_STAGE/etc/passwd"
printf 'root:x:0:\nnogroup:x:65534:\n' >"$ROOTFS_STAGE/etc/group"
echo "  [/etc/passwd + group created]"

# 用 staging 目录直接构建 ext4 镜像，无需挂载
if [ "$ROOTFS_MBR" = "1" ]; then
    "$SCRIPT_DIR/make_mbr_image.sh" "$ROOTFS_STAGE" "$ROOTFS_IMG" "$ROOTFS_SIZE_MB" "$FAT_PART_MB"
else
    dd if=/dev/zero of="$ROOTFS_IMG" bs=1M count="$ROOTFS_SIZE_MB" status=none
    mkfs.ext4 -q -b 1024 -L "avatarfs" -d "$ROOTFS_STAGE" "$ROOTFS_IMG"
fi
rm -rf "$ROOTFS_STAGE"
echo "Rootfs ready: $ROOTFS_IMG"
