#!/bin/bash
#
# install-apps.sh - 安装应用程序到 rootfs
#
# 用法: ./install-apps.sh [架构]
#
# 示例: ./install-apps.sh aarch64

ARCH=${1:-aarch64}
case "$ARCH" in
    ARCH=*) ARCH="${ARCH#ARCH=}" ;;
esac
ROOTFS_IMG="build/rootfs.img"
MOUNT_POINT="/tmp/avatar_mnt"

echo "=================================="
echo "Installing applications to rootfs"
echo "=================================="
echo ""

# 检查 rootfs 是否存在
if [ ! -f "$ROOTFS_IMG" ]; then
    echo "Error: $ROOTFS_IMG not found!"
    echo "Please run: make ARCH=$ARCH rootfs"
    exit 1
fi

# 创建挂载点
echo "Creating mount point: $MOUNT_POINT"
mkdir -p "$MOUNT_POINT"

# 挂载 rootfs
echo "Mounting $ROOTFS_IMG to $MOUNT_POINT"
sudo mount -o loop "$ROOTFS_IMG" "$MOUNT_POINT"
if [ $? -ne 0 ]; then
    echo "Error: Failed to mount $ROOTFS_IMG"
    rmdir "$MOUNT_POINT"
    exit 1
fi

# 复制应用程序（优先安装 ELF，其次才是裸 .bin）
echo ""
echo "Installing applications:"
INSTALLED=0

# 1) 安装由 .S 生成的 ELF（如 build/hello.bin.elf -> /hello）
for ELF in build/*.bin.elf; do
    [ -f "$ELF" ] || continue
    APP_NAME=$(basename "$ELF" .bin.elf)
    echo "  - $APP_NAME  ($ELF)"
    sudo cp "$ELF" "$MOUNT_POINT/$APP_NAME"
    sudo chmod +x "$MOUNT_POINT/$APP_NAME"
    INSTALLED=$((INSTALLED + 1))
done

# 2) 安装 C 用户程序 ELF（如 build/init.elf -> /init）
for ELF in build/*.elf; do
    [ -f "$ELF" ] || continue
    case "$ELF" in
        *.bin.elf) continue ;;
    esac
    APP_NAME=$(basename "$ELF" .elf)
    echo "  - $APP_NAME  ($ELF)"
    sudo cp "$ELF" "$MOUNT_POINT/$APP_NAME"
    sudo chmod +x "$MOUNT_POINT/$APP_NAME"
    INSTALLED=$((INSTALLED + 1))
done

# 3) 兜底：仅当没有 ELF 可安装时，才安装裸 .bin
if [ "$INSTALLED" -eq 0 ]; then
for BIN in build/*.bin; do
    [ -f "$BIN" ] || continue
    # 去掉 build/ 前缀和 .bin 后缀作为目标文件名
    APP_NAME=$(basename "$BIN" .bin)
    echo "  - $APP_NAME  ($BIN)"
    sudo cp "$BIN" "$MOUNT_POINT/$APP_NAME"
    sudo chmod +x "$MOUNT_POINT/$APP_NAME"
    INSTALLED=$((INSTALLED + 1))
done
fi
if [ "$INSTALLED" -eq 0 ]; then
    echo "  Warning: No app artifacts found in build/"
    echo "  Please run: make ARCH=$ARCH rootfs"
fi

# 安装 busybox（如果存在）
echo ""
BUSYBOX_SRC="apps/busybox-$ARCH"
if [ -f "$BUSYBOX_SRC" ]; then
    echo "  - busybox  ($BUSYBOX_SRC)"
    sudo cp "$BUSYBOX_SRC" "$MOUNT_POINT/busybox"
    sudo chmod +x "$MOUNT_POINT/busybox"
    INSTALLED=$((INSTALLED + 1))
    echo "  [busybox installed successfully]"
else
    echo "  Warning: busybox not found at $BUSYBOX_SRC"
    echo "  To build busybox, see: apps/busybox-1.37.0/"
fi

# 列出安装的文件
echo ""
echo "Files in rootfs:"
sudo ls -la "$MOUNT_POINT"

# 卸载
echo ""
echo "Unmounting $MOUNT_POINT"
sudo umount "$MOUNT_POINT"

# 清理
rmdir "$MOUNT_POINT"

echo ""
echo "=================================="
echo "Installation complete!"
echo "=================================="
echo ""
echo "Now run: make ARCH=$ARCH run-fs"
echo ""
