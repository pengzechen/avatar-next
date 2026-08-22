#!/bin/bash
#
# install-apps.sh - 构建 rootfs 镜像并安装应用程序（无需 sudo）
#
# 用法: ./install-apps.sh [ARCH=]<架构>
#
# 示例:
#   ./install-apps.sh aarch64
#   ./install-apps.sh ARCH=riscv64
#
# 原理：使用 mkfs.ext4 -d <staging_dir> 直接从目录树构建 ext4 镜像，
#       完全不需要 sudo / loop mount。
#       需要 e2fsprogs >= 1.43（提供 mkfs.ext4 -d 选项）。

set -e

ARCH=${1:-aarch64}
case "$ARCH" in
    ARCH=*) ARCH="${ARCH#ARCH=}" ;;
esac

BUILD_DIR="build"
ROOTFS_IMG="${BUILD_DIR}/rootfs-${ARCH}.img"
STAGE_DIR="${BUILD_DIR}/rootfs-stage-${ARCH}"

# 从 mem_layout.mk 读取镜像大小（优先），否则默认 32 MB
ROOTFS_SIZE_MB=32
if [ -f "${BUILD_DIR}/mem_layout.mk" ]; then
    _sz=$(grep 'ROOTFS_SIZE_MB' "${BUILD_DIR}/mem_layout.mk" | head -1 | sed 's/.*= *//')
    [ -n "$_sz" ] && ROOTFS_SIZE_MB=$_sz
fi

echo "=================================="
echo " Building rootfs for $ARCH"
echo " Image : $ROOTFS_IMG  (${ROOTFS_SIZE_MB} MB)"
echo "=================================="
echo ""

# ── 检查 mkfs.ext4 是否支持 -d ─────────────────────────────────────
if ! mkfs.ext4 --help 2>&1 | grep -q -- '-d '; then
    echo "ERROR: mkfs.ext4 does not support -d option."
    echo "       Please upgrade e2fsprogs to >= 1.43."
    exit 1
fi

# ── 创建 staging 目录 ───────────────────────────────────────────────
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR/bin"

INSTALLED=0

# ── 安装 busybox ────────────────────────────────────────────────────
BUSYBOX_SRC="apps/busybox-${ARCH}"
if [ -f "$BUSYBOX_SRC" ]; then
    cp "$BUSYBOX_SRC" "$STAGE_DIR/busybox"
    chmod +x "$STAGE_DIR/busybox"
    for applet in sh ls cat echo pwd mkdir rm cp mv grep find ps kill vi more less head tail wc sleep date stty clear; do
        cp "$BUSYBOX_SRC" "$STAGE_DIR/bin/$applet"
        chmod +x "$STAGE_DIR/bin/$applet"
    done
    echo "  [busybox + applets installed]"
    INSTALLED=$((INSTALLED + 1))
else
    echo "  Warning: busybox not found at $BUSYBOX_SRC"
fi

# ── 安装 ELF（.bin.elf 优先，然后普通 .elf，兜底裸 .bin）────────────
for ELF in "${BUILD_DIR}"/*.bin.elf; do
    [ -f "$ELF" ] || continue
    NAME=$(basename "$ELF" .bin.elf)
    cp "$ELF" "$STAGE_DIR/$NAME"
    chmod +x "$STAGE_DIR/$NAME"
    echo "  - $NAME  ($ELF)"
    INSTALLED=$((INSTALLED + 1))
done

for ELF in "${BUILD_DIR}"/*.elf; do
    [ -f "$ELF" ] || continue
    case "$ELF" in *.bin.elf) continue ;; esac
    NAME=$(basename "$ELF" .elf)
    cp "$ELF" "$STAGE_DIR/$NAME"
    chmod +x "$STAGE_DIR/$NAME"
    echo "  - $NAME  ($ELF)"
    INSTALLED=$((INSTALLED + 1))
done

if [ "$INSTALLED" -le 1 ]; then   # 只装了 busybox，还没有用户 ELF
    for BIN in "${BUILD_DIR}"/*.bin; do
        [ -f "$BIN" ] || continue
        NAME=$(basename "$BIN" .bin)
        cp "$BIN" "$STAGE_DIR/$NAME"
        chmod +x "$STAGE_DIR/$NAME"
        echo "  - $NAME  ($BIN)"
        INSTALLED=$((INSTALLED + 1))
    done
fi

if [ "$INSTALLED" -eq 0 ]; then
    echo "  Warning: No app artifacts found in build/"
    echo "  Please run: make PLATFORM=qemu-virt-$ARCH first"
fi

# ── 构建 ext4 镜像（无需 mount / sudo）─────────────────────────────
echo ""
echo "Building ext4 image from staging dir..."
dd if=/dev/zero of="$ROOTFS_IMG" bs=1M count="$ROOTFS_SIZE_MB" status=none
mkfs.ext4 -q -b 1024 -L "avatarfs" -d "$STAGE_DIR" "$ROOTFS_IMG"

# ── 清理 staging ────────────────────────────────────────────────────
rm -rf "$STAGE_DIR"

echo ""
echo "=================================="
echo " Installation complete!"
echo " $ROOTFS_IMG"
echo "=================================="
echo ""
echo "Now run: make PLATFORM=qemu-virt-$ARCH run-fs"
echo ""
