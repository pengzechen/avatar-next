#!/usr/bin/env bash
# build-dynamic-busybox.sh
# 编译三份动态链接 BusyBox，并组装含 musl 库的 rootfs staging
#
# 用法：
#   cd avatar/apps
#   bash build-dynamic-busybox.sh
#
# 产物：
#   apps/{aarch64,riscv64,x86_64}/rootfs/   — rootfs staging 目录
#   apps/busybox-dynamic-{aarch64,riscv64,x86_64} — 单独的 busybox 可执行文件

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BB_TARBALL="$SCRIPT_DIR/busybox-1.37.0.tar.bz2"
BUILD_BASE="/tmp/bb-dynamic-build"
JOBS="$(nproc)"

# ─── 工具链 ──────────────────────────────────────────────────────────────────
declare -A CROSS=(
  [aarch64]="aarch64-linux-musl-"
  [riscv64]="riscv64-linux-musl-"
  [x86_64]="x86_64-linux-musl-"
)

declare -A SYSROOT=(
  [aarch64]="$HOME/SoftWare/compiler/aarch64-linux-musl-cross/aarch64-linux-musl"
  [riscv64]="$HOME/SoftWare/compiler/riscv64-linux-musl-cross/riscv64-linux-musl"
  [x86_64]="$HOME/SoftWare/compiler/x86_64-linux-musl-cross/x86_64-linux-musl"
)

declare -A LDSO=(
  [aarch64]="ld-musl-aarch64.so.1"
  [riscv64]="ld-musl-riscv64.so.1"
  [x86_64]="ld-musl-x86_64.so.1"
)

# ─── 工具 ────────────────────────────────────────────────────────────────────
die()  { echo "[ERROR] $*" >&2; exit 1; }
info() { echo "[INFO]  $*"; }

require_cmd() {
  for c in "$@"; do
    command -v "$c" >/dev/null 2>&1 || die "Command not found: $c"
  done
}

# ─── 前置检查 ─────────────────────────────────────────────────────────────────
require_cmd tar sed make strip
[ -f "$BB_TARBALL" ] || die "BusyBox tarball not found: $BB_TARBALL"

for arch in aarch64 riscv64 x86_64; do
  cc="${CROSS[$arch]}gcc"
  command -v "$cc" >/dev/null 2>&1 || die "Cross compiler not found: $cc"
  [ -d "${SYSROOT[$arch]}" ]       || die "Sysroot not found: ${SYSROOT[$arch]}"
done

# ─── 构建函数 ─────────────────────────────────────────────────────────────────
build_busybox() {
  local arch="$1"
  local cross="${CROSS[$arch]}"
  local srcdir="$BUILD_BASE/$arch"
  local rootfs="$SCRIPT_DIR/$arch/rootfs"
  local sysroot="${SYSROOT[$arch]}"
  local ldso="${LDSO[$arch]}"

  info "===== [$arch] 开始构建 ====="

  # ── 解压源码 ──
  rm -rf "$srcdir"
  mkdir -p "$srcdir"
  info "[$arch] 解压 BusyBox 源码..."
  tar -xjf "$BB_TARBALL" -C "$srcdir" --strip-components=1

  # ── 生成 defconfig 并关闭静态链接 ──
  info "[$arch] 配置..."
  make -C "$srcdir" CROSS_COMPILE="$cross" defconfig -j"$JOBS" >/dev/null 2>&1

  # 关闭 CONFIG_STATIC / CONFIG_STATIC_LIBGCC
  sed -i 's/^CONFIG_STATIC=y/# CONFIG_STATIC is not set/'          "$srcdir/.config"
  sed -i 's/^CONFIG_STATIC_LIBGCC=y/# CONFIG_STATIC_LIBGCC is not set/' "$srcdir/.config"

  # 关闭一些在裸机内核中大概率不支持的特性（减少运行时问题）
  for opt in \
    FEATURE_WTMP FEATURE_LASTLOG \
    SELINUX SMACK \
    FEATURE_DEVMEM DEVMEM DEVKMEM \
    FEATURE_MOUNT_NFS \
    PAM \
    SHA1SUM SHA256SUM SHA512SUM SHA3SUM MD5SUM \
    FEATURE_SHA1_HWACCEL FEATURE_SHA256_HWACCEL; do
    sed -i "s/^CONFIG_${opt}=y/# CONFIG_${opt} is not set/" "$srcdir/.config" || true
  done

  yes "" | make -C "$srcdir" CROSS_COMPILE="$cross" oldconfig >/dev/null 2>&1

  # ── 编译 ──
  info "[$arch] 编译（-j${JOBS}）..."
  make -C "$srcdir" CROSS_COMPILE="$cross" -j"$JOBS" \
    2>&1 | grep -E "^(CC|LD|LINK|error:|warning: )" | tail -20 || true

  # ── 验证产物 ──
  local bb_bin="$srcdir/busybox"
  [ -f "$bb_bin" ] || die "[$arch] busybox binary not built"
  file "$bb_bin" | grep -q "dynamically linked" \
    || { echo "[WARN] [$arch] busybox is NOT dynamically linked – check config"; }

  # ── 复制到 apps/ ──
  cp "$bb_bin" "$SCRIPT_DIR/busybox-dynamic-$arch"
  info "[$arch] 输出: $SCRIPT_DIR/busybox-dynamic-$arch"

  # ── 组装 rootfs staging ──
  info "[$arch] 组装 rootfs staging..."
  rm -rf "$rootfs"
  mkdir -p "$rootfs"/{bin,sbin,lib,usr/bin,usr/sbin,usr/lib,etc,proc,sys,dev,tmp,var/log,root}

  # busybox 安装（通过 install 目标创建所有 applet 符号链接）
  make -C "$srcdir" CROSS_COMPILE="$cross" CONFIG_PREFIX="$rootfs" install \
    >/dev/null 2>&1
  # 确保主体也在 bin/
  cp "$bb_bin" "$rootfs/bin/busybox"

  # ── 复制 musl 运行时库 ──
  local slib="$sysroot/lib"
  info "[$arch] 复制 musl 运行时库..."

  # libc.so（musl，同时充当 loader 本体）
  cp -L "$slib/libc.so"           "$rootfs/lib/libc.so"

  # ld-musl-<arch>.so.1 → libc.so（符号链接）
  ln -sf libc.so "$rootfs/lib/$ldso"

  # libgcc_s（GCC 运行时，busybox 某些功能依赖）
  cp "$slib/libgcc_s.so.1"        "$rootfs/lib/libgcc_s.so.1"
  ln -sf libgcc_s.so.1             "$rootfs/lib/libgcc_s.so"

  # libstdc++（C++ 运行时）
  local stdcxx_real
  stdcxx_real=$(ls "$slib"/libstdc++.so.6.*.* 2>/dev/null | grep -v gdb.py | head -1)
  if [ -n "$stdcxx_real" ]; then
    cp "$stdcxx_real"              "$rootfs/lib/$(basename "$stdcxx_real")"
    ln -sf "$(basename "$stdcxx_real")" "$rootfs/lib/libstdc++.so.6"
    ln -sf "$(basename "$stdcxx_real")" "$rootfs/lib/libstdc++.so"
  fi

  # libm / libpthread / libdl / librt — 在 musl 里都合并进 libc.so，
  # 但有些程序期望这些符号链接存在（glibc 遗留依赖），给个空符号链接
  for stub_lib in libm.so.6 libpthread.so.0 libdl.so.2 librt.so.1; do
    ln -sf libc.so "$rootfs/lib/$stub_lib" || true
  done

  # ── /etc 基本文件 ──
  cat > "$rootfs/etc/passwd" <<'EOF'
root::0:0:root:/root:/bin/sh
EOF
  cat > "$rootfs/etc/group" <<'EOF'
root:x:0:
EOF
  cat > "$rootfs/etc/hostname" <<'EOF'
avatar
EOF

  # ── /init ──
  cat > "$rootfs/init" <<'EOF'
#!/bin/sh
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
mount -t proc proc /proc 2>/dev/null || true
mount -t sysfs sysfs /sys 2>/dev/null || true
echo "Avatar OS - dynamic busybox rootfs (ARCH_PLACEHOLDER)"
exec /bin/sh
EOF
  sed -i "s/ARCH_PLACEHOLDER/$arch/" "$rootfs/init"
  chmod +x "$rootfs/init"

  # ── strip 减小体积（保留 debug info 用 --strip-unneeded） ──
  ${cross}strip --strip-unneeded "$rootfs/bin/busybox" 2>/dev/null || true
  ${cross}strip --strip-unneeded "$rootfs/lib/libc.so" 2>/dev/null || true
  ${cross}strip --strip-unneeded "$rootfs/lib/libgcc_s.so.1" 2>/dev/null || true

  info "[$arch] rootfs: $rootfs"
  info "[$arch] 大小概览:"
  du -sh "$rootfs" "$rootfs/lib" "$rootfs/bin" 2>/dev/null | head -10

  info "[$arch] 库列表:"
  ls -lh "$rootfs/lib/"

  info "===== [$arch] 完成 ====="
  echo ""
}

# ─── 主流程 ───────────────────────────────────────────────────────────────────
for arch in aarch64 riscv64 x86_64; do
  build_busybox "$arch"
done

info "全部完成。产物："
for arch in aarch64 riscv64 x86_64; do
  echo "  $arch:"
  echo "    binary : apps/busybox-dynamic-$arch"
  echo "    rootfs : apps/$arch/rootfs/"
done
