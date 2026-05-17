#!/bin/bash
# apps/c/build.sh
# 将 apps/c/pthread/test.c 和 apps/c/mutex/test.c 交叉编译为三个架构
# 并打包进 imgs/*.img
#
# 用法:  cd avatar && bash apps/c/build.sh
#        或者加 -v 显示详细编译命令

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
COMPILER_BASE="$HOME/SoftWare/compiler"

VERBOSE=0
[[ "${1:-}" == "-v" ]] && VERBOSE=1

info()  { printf '\033[1;34m[info]\033[0m  %s\n' "$*"; }
ok()    { printf '\033[1;32m[ ok ]\033[0m  %s\n' "$*"; }
err()   { printf '\033[1;31m[err ]\033[0m  %s\n' "$*" >&2; }

# ── 编译单个程序到指定架构 ────────────────────────────────────────────
#   build_prog <arch> <cc_prefix> <ld_interp> <src_file> <bin_name>
build_prog() {
    local arch="$1"
    local cc_prefix="$2"
    local ld_interp="$3"
    local src="$4"
    local bin_name="$5"

    local sysroot="$COMPILER_BASE/${cc_prefix}-cross/${cc_prefix}"
    local cc="${cc_prefix}-gcc"
    local staging="$ROOT_DIR/apps/${arch}/rootfs"
    local bin_dir="$staging/bin"
    local out_bin="$SCRIPT_DIR/${bin_name}-${arch}"

    info "[$arch] 编译 $bin_name ..."

    if ! command -v "$cc" &>/dev/null; then
        err "[$arch] 找不到编译器 $cc，跳过"
        return 1
    fi

    local cflags=(
        -O1 -g
        -Wall -Wextra
        -I"$sysroot/include"
        -L"$sysroot/lib"
        -Wl,-dynamic-linker,"/lib/${ld_interp}"
        -Wl,-rpath,"/lib"
        -lpthread
    )

    if [[ $VERBOSE -eq 1 ]]; then
        echo "  $cc ${cflags[*]} $src -o $out_bin"
    fi

    if ! "$cc" "${cflags[@]}" "$src" -o "$out_bin"; then
        err "[$arch] $bin_name 编译失败"
        return 1
    fi

    mkdir -p "$bin_dir"
    cp "$out_bin" "$bin_dir/$bin_name"
    chmod 755 "$bin_dir/$bin_name"
    ok "[$arch] 已安装 → $bin_dir/$bin_name"
}

# ── 重建 ext4 镜像 ────────────────────────────────────────────────────
rebuild_img() {
    local arch="$1"
    local staging="$ROOT_DIR/apps/${arch}/rootfs"
    local img="$ROOT_DIR/imgs/rootfs-${arch}.img"

    info "[$arch] 重建 $img ..."

    if [[ ! -d "$staging" ]]; then
        err "[$arch] staging 目录不存在: $staging"
        return 1
    fi

    dd if=/dev/zero of="$img" bs=1M count=64 status=none
    mkfs.ext4 -q -b 1024 -L "avatarfs" -d "$staging" "$img" 2>/dev/null
    ok "[$arch] 镜像重建完成: $img ($(du -h "$img" | cut -f1))"
}

# ── 主流程 ───────────────────────────────────────────────────────────
main() {
    info "apps/c/pthread/test.c  → /bin/pthread_test"
    info "apps/c/mutex/test.c    → /bin/mutex_test"
    echo

    local ok_count=0
    local fail_count=0

    declare -A LD_INTERP=(
        [aarch64]="ld-musl-aarch64.so.1"
        [riscv64]="ld-musl-riscv64.so.1"
        [x86_64]="ld-musl-x86_64.so.1"
    )
    declare -A CC_PREFIX=(
        [aarch64]="aarch64-linux-musl"
        [riscv64]="riscv64-linux-musl"
        [x86_64]="x86_64-linux-musl"
    )

    for arch in aarch64 riscv64 x86_64; do
        echo "──────────────────────────────────────────"
        local arch_ok=1

        build_prog "$arch" "${CC_PREFIX[$arch]}" "${LD_INTERP[$arch]}" \
            "$SCRIPT_DIR/pthread/test.c" "pthread_test" || arch_ok=0

        build_prog "$arch" "${CC_PREFIX[$arch]}" "${LD_INTERP[$arch]}" \
            "$SCRIPT_DIR/mutex/test.c" "mutex_test" || arch_ok=0

        if [[ $arch_ok -eq 1 ]]; then
            rebuild_img "$arch" && ((ok_count++)) || true
        else
            ((fail_count++)) || true
        fi
        echo
    done

    echo "══════════════════════════════════════════"
    if [[ $fail_count -eq 0 ]]; then
        ok "全部完成 ($ok_count / 3 架构)"
    else
        err "部分失败 (ok=$ok_count fail=$fail_count)"
        exit 1
    fi
}

main "$@"

