#!/usr/bin/env bash
# Build a RISC-V64 musl C development overlay for the Avatar OS rootfs.
#
# The host cross compiler is x86-hosted and cannot run on the board. This script
# packages the musl sysroot and builds a small native RISC-V64 TinyCC frontend so
# the board can compile simple C programs against musl:
#
#   cc hello.c -o hello
#   cc -x c 1.txt -o hello

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
COMPILER_BASE="${COMPILER_BASE:-$HOME/Desktop/Software/compiler}"
CROSS_PREFIX="${CROSS_PREFIX:-riscv64-linux-musl}"
CROSS_ROOT="${CROSS_ROOT:-$COMPILER_BASE/${CROSS_PREFIX}-cross}"
SYSROOT="${SYSROOT:-$CROSS_ROOT/$CROSS_PREFIX}"
TCC_SRC="${TCC_SRC:-/tmp/opencode/tinycc}"
TCC_BUILD="${TCC_BUILD:-/tmp/opencode/tinycc-rv64-build}"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/build/rv64-musl-dev-rootfs}"
TAR_OUT="${TAR_OUT:-$ROOT_DIR/build/rv64-musl-dev-rootfs.tar.gz}"

die() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "missing command: $1"
}

need_cmd git
need_cmd make
need_cmd gcc
need_cmd tar
need_cmd "${CROSS_PREFIX}-gcc"
need_cmd "${CROSS_PREFIX}-ar"
need_cmd "${CROSS_PREFIX}-strip"

[ -d "$SYSROOT/include" ] || die "sysroot include not found: $SYSROOT/include"
[ -d "$SYSROOT/lib" ] || die "sysroot lib not found: $SYSROOT/lib"

mkdir -p /tmp/opencode
if [ ! -d "$TCC_SRC/.git" ]; then
    rm -rf "$TCC_SRC"
    git clone --depth 1 https://repo.or.cz/tinycc.git "$TCC_SRC"
fi

rm -rf "$TCC_BUILD"
mkdir -p "$TCC_BUILD"

(
    cd "$TCC_BUILD"
    "$TCC_SRC/configure" \
        --source-path="$TCC_SRC" \
        --cc="${CROSS_PREFIX}-gcc" \
        --ar="${CROSS_PREFIX}-ar" \
        --cpu=riscv64 \
        --targetos=Linux \
        --prefix=/usr \
        --sysincludepaths=/usr/include:/include \
        --libpaths=/usr/lib:/lib \
        --crtprefix=/usr/lib:/lib \
        --elfinterp=/lib/ld-musl-riscv64v0p7_xthead.so.1 \
        --extra-cflags='-static -no-pie -fno-pie -O2' \
        --extra-ldflags='-static -no-pie'

    gcc -DC2STR "$TCC_SRC/conftest.c" -o c2str.exe
    ./c2str.exe "$TCC_SRC/include/tccdefs.h" tccdefs_.h
    make -j"$(nproc)" tcc
)

LIBTCC1_WORK="$TCC_BUILD/libtcc1-work"
rm -rf "$LIBTCC1_WORK"
mkdir -p "$LIBTCC1_WORK"

COMMON_CFLAGS=(-I"$TCC_BUILD" -I"$TCC_SRC" -fPIC -fno-omit-frame-pointer -Wno-unused-function -Wno-unused-variable)
"${CROSS_PREFIX}-gcc" -c "$TCC_SRC/lib/lib-arm64.c" -o "$LIBTCC1_WORK/lib-arm64.o" "${COMMON_CFLAGS[@]}"
"${CROSS_PREFIX}-gcc" -c "$TCC_SRC/lib/stdatomic.c" -o "$LIBTCC1_WORK/stdatomic.o" "${COMMON_CFLAGS[@]}"
"${CROSS_PREFIX}-gcc" -c "$TCC_SRC/lib/atomic.S" -o "$LIBTCC1_WORK/atomic.o" -I"$TCC_BUILD" -I"$TCC_SRC" -fPIC -fno-omit-frame-pointer
"${CROSS_PREFIX}-gcc" -c "$TCC_SRC/lib/builtin.c" -o "$LIBTCC1_WORK/builtin.o" "${COMMON_CFLAGS[@]}"
"${CROSS_PREFIX}-gcc" -c "$TCC_SRC/lib/alloca.S" -o "$LIBTCC1_WORK/alloca.o" -I"$TCC_BUILD" -I"$TCC_SRC" -fPIC -fno-omit-frame-pointer
"${CROSS_PREFIX}-gcc" -c "$TCC_SRC/lib/alloca-bt.S" -o "$LIBTCC1_WORK/alloca-bt.o" -I"$TCC_BUILD" -I"$TCC_SRC" -fPIC -fno-omit-frame-pointer
"${CROSS_PREFIX}-gcc" -c "$TCC_SRC/lib/armflush.c" -o "$LIBTCC1_WORK/armflush.o" "${COMMON_CFLAGS[@]}"
"${CROSS_PREFIX}-gcc" -c "$TCC_SRC/lib/dsohandle.c" -o "$LIBTCC1_WORK/dsohandle.o" "${COMMON_CFLAGS[@]}"
"${CROSS_PREFIX}-ar" rcs "$TCC_BUILD/libtcc1.a" "$LIBTCC1_WORK"/*.o

rm -rf "$OUT_DIR" "$TAR_OUT"
mkdir -p "$OUT_DIR/bin" "$OUT_DIR/lib" "$OUT_DIR/usr/bin" "$OUT_DIR/usr/include" "$OUT_DIR/usr/lib/tcc" "$OUT_DIR/usr/lib"

"${CROSS_PREFIX}-strip" -o "$OUT_DIR/usr/bin/tcc" "$TCC_BUILD/tcc"
chmod 755 "$OUT_DIR/usr/bin/tcc"
ln -sf tcc "$OUT_DIR/usr/bin/cc"
ln -sf tcc "$OUT_DIR/usr/bin/musl-cc"

cp -a "$SYSROOT/include/." "$OUT_DIR/usr/include/"
cp -a "$SYSROOT/lib/." "$OUT_DIR/usr/lib/"
cp "$TCC_BUILD/libtcc1.a" "$OUT_DIR/usr/lib/tcc/libtcc1.a"
cp "$TCC_BUILD/libtcc1.a" "$OUT_DIR/usr/lib/libtcc1.a"

# Runtime loader/library aliases used by existing SG2002 rootfs experiments.
cp -L "$SYSROOT/lib/libc.so" "$OUT_DIR/lib/libc.so"
ln -sf libc.so "$OUT_DIR/lib/ld-musl-riscv64.so.1"
ln -sf libc.so "$OUT_DIR/lib/ld-musl-riscv64v0p7_xthead.so.1"
if [ -f "$SYSROOT/lib/libgcc_s.so.1" ]; then
    cp -L "$SYSROOT/lib/libgcc_s.so.1" "$OUT_DIR/lib/libgcc_s.so.1"
    ln -sf libgcc_s.so.1 "$OUT_DIR/lib/libgcc_s.so"
fi

# Convenience smoke test source for the board.
cat > "$OUT_DIR/root-hello.c" <<'EOF'
#include <stdio.h>

int main(void) {
    printf("hello world!\n");
    return 0;
}
EOF

tar -C "$OUT_DIR" -czf "$TAR_OUT" .

printf 'Overlay ready:\n'
printf '  tree: %s\n' "$OUT_DIR"
printf '  tar : %s\n' "$TAR_OUT"
du -sh "$OUT_DIR" "$TAR_OUT"
