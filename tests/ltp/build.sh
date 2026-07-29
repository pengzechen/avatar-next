#!/bin/bash
# tests/ltp/build.sh — 交叉编译选定的 LTP 测例为 riscv64 静态二进制
#
# 用法:  bash tests/ltp/build.sh [ARCH]
#        ARCH 默认 riscv64，也支持 aarch64 / x86_64
#
# 前提:  third_party/ltp/ 下已有 LTP release tarball 解压后的源码
# 产物:  tests/ltp/bin/<arch>/ 下的静态 ELF 二进制

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
LTP_DIR="$ROOT_DIR/third_party/ltp"
TESTCASES_LIST="$SCRIPT_DIR/testcases.list"

ARCH="${1:-riscv64}"

# ── 工具链映射 ──────────────────────────────────────────────────────
case "$ARCH" in
    riscv64)  CC_PREFIX="riscv64-linux-musl" ;;
    aarch64)  CC_PREFIX="aarch64-linux-musl" ;;
    x86_64)   CC_PREFIX="x86_64-linux-musl"  ;;
    *)        echo "ERROR: unsupported ARCH=$ARCH"; exit 1 ;;
esac

CC="${CC_PREFIX}-gcc"
if ! command -v "$CC" &>/dev/null; then
    echo "ERROR: compiler $CC not found in PATH"
    exit 1
fi

OUT_DIR="$SCRIPT_DIR/bin/$ARCH"
mkdir -p "$OUT_DIR"
rm -f "$OUT_DIR"/*

info()  { printf '\033[1;34m[ltp]\033[0m  %s\n' "$*"; }
ok()    { printf '\033[1;32m[ ok]\033[0m  %s\n' "$*"; }
err()   { printf '\033[1;31m[err]\033[0m  %s\n' "$*" >&2; }

# ── Step 1: Configure LTP (重新 configure 当架构变化时) ─────────────
NEED_CONFIGURE=0
if [ ! -f "$LTP_DIR/include/config.h" ]; then
    NEED_CONFIGURE=1
elif grep -q "host_cpu *= *${ARCH%%64}" "$LTP_DIR/config.status" 2>/dev/null; then
    info "LTP already configured for $ARCH"
elif grep -q "host_alias *= *${CC_PREFIX}" "$LTP_DIR/config.status" 2>/dev/null; then
    info "LTP already configured for $ARCH"
else
    info "Architecture changed, reconfiguring ..."
    make -C "$LTP_DIR" distclean > /dev/null 2>&1 || true
    NEED_CONFIGURE=1
fi

if [ "$NEED_CONFIGURE" -eq 1 ]; then
    info "Configuring LTP for $ARCH ..."
    (
        cd "$LTP_DIR"
        ./configure \
            --host="$CC_PREFIX" \
            CC="$CC" \
            LDFLAGS="-static" \
            --prefix=/opt/ltp \
            > /dev/null 2>&1
    )
    ok "Configure done"
fi

# ── Step 2: Build libltp ────────────────────────────────────────────
info "Building libltp ..."
make -C "$LTP_DIR/lib" -j"$(nproc)" > /dev/null 2>&1
ok "libltp built"

# ── Step 3: Build selected testcases ────────────────────────────────
TOTAL=0
BUILT=0
FAILED=0

while IFS= read -r line; do
    # 跳过空行和注释
    line="${line%%#*}"
    line="$(echo "$line" | xargs)"
    [ -z "$line" ] && continue

    dir="${line%/*}"
    bin="${line##*/}"
    tc_dir="$LTP_DIR/testcases/kernel/syscalls/$dir"

    if [ ! -d "$tc_dir" ]; then
        err "Directory not found: $tc_dir"
        ((FAILED++)) || true
        ((TOTAL++)) || true
        continue
    fi

    ((TOTAL++)) || true
    info "Building $line ..."

    # make -k: 即使同目录其他测例失败也继续编译目标测例
    make -C "$tc_dir" -j"$(nproc)" -k > /dev/null 2>&1 || true

    if [ -f "$tc_dir/$bin" ]; then
        cp "$tc_dir/$bin" "$OUT_DIR/"
        ok "$bin → $OUT_DIR/$bin"
        ((BUILT++)) || true
    else
        err "Build failed: $line"
        ((FAILED++)) || true
    fi
done < "$TESTCASES_LIST"

# ── Step 4: Copy run script ────────────────────────────────────────
cp "$SCRIPT_DIR/run_ltp.sh" "$OUT_DIR/"
chmod +x "$OUT_DIR/run_ltp.sh"

# ── Summary ─────────────────────────────────────────────────────────
echo ""
echo "══════════════════════════════════════════"
if [ "$FAILED" -eq 0 ]; then
    ok "All $BUILT/$TOTAL testcases built for $ARCH"
else
    err "Built $BUILT/$TOTAL, failed $FAILED"
    exit 1
fi
echo "Output: $OUT_DIR/"
