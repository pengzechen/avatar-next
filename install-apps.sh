#!/bin/bash
#
# install-apps.sh - 安装应用程序到 rootfs
#
# 用法: ./install-apps.sh [架构]
#
# 示例: ./install-apps.sh aarch64

ARCH=${1:-aarch64}
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

# 复制应用程序（安装 build/ 下所有 .bin 文件）
echo ""
echo "Installing applications:"
INSTALLED=0
for BIN in build/*.bin; do
    [ -f "$BIN" ] || continue
    # 去掉 build/ 前缀和 .bin 后缀作为目标文件名
    APP_NAME=$(basename "$BIN" .bin)
    echo "  - $APP_NAME  ($BIN)"
    sudo cp "$BIN" "$MOUNT_POINT/$APP_NAME"
    sudo chmod +x "$MOUNT_POINT/$APP_NAME"
    INSTALLED=$((INSTALLED + 1))
done
if [ "$INSTALLED" -eq 0 ]; then
    echo "  Warning: No .bin files found in build/"
    echo "  Please run: make ARCH=$ARCH rootfs"
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
