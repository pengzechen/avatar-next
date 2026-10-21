#!/bin/bash
# ci_setup_hosted.sh — GitHub-hosted runner（ubuntu-latest）准备脚本
#
# 安装 QEMU / e2fsprogs，并把 musl 交叉工具链下载到缓存目录。
# 上传后的 PATH 通过 $GITHUB_PATH 传给后续 step。
#
# 本地调试：直接运行（没有 GITHUB_PATH 时只导出到当前 shell）。
set -euo pipefail

TOOL_DIR="${AVATAR_TOOLCHAIN_DIR:-$HOME/.avatar-toolchains}"
mkdir -p "$TOOL_DIR"

info() { printf '\033[1;34m[ci]\033[0m %s\n' "$*"; }

# QEMU：aarch64 在 qemu-system-arm，riscv64 在 qemu-system-misc，x86 在 qemu-system-x86。
if ! command -v qemu-system-aarch64 >/dev/null 2>&1 ||
   ! command -v qemu-system-riscv64 >/dev/null 2>&1 ||
   ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
    info "installing QEMU + e2fsprogs ..."
    sudo apt-get update -qq
    sudo apt-get install -y -qq \
        qemu-system-arm qemu-system-misc qemu-system-x86 \
        e2fsprogs xz-utils >/dev/null
fi

for arch in aarch64 riscv64 x86_64; do
    if command -v "${arch}-linux-musl-gcc" >/dev/null 2>&1; then
        info "${arch}-linux-musl-gcc already on PATH"
        continue
    fi
    tarball="${arch}-linux-musl-cross"
    if [ ! -x "$TOOL_DIR/$tarball/bin/${arch}-linux-musl-gcc" ]; then
        info "downloading $tarball ..."
        curl -fSL --retry 3 \
            "https://musl.cc/${tarball}.tgz" \
            -o "$TOOL_DIR/${tarball}.tgz"
        tar -xzf "$TOOL_DIR/${tarball}.tgz" -C "$TOOL_DIR"
        rm -f "$TOOL_DIR/${tarball}.tgz"
    fi
done

if [ -n "${GITHUB_PATH:-}" ]; then
    for d in "$TOOL_DIR"/*-linux-musl-cross/bin; do
        [ -d "$d" ] && printf '%s\n' "$d" >> "$GITHUB_PATH"
    done
else
    export PATH="$TOOL_DIR"/*-linux-musl-cross/bin:"$PATH"
fi

if [ -n "${GITHUB_ENV:-}" ]; then
    printf 'AVATAR_TOOLCHAIN_DIR=%s\n' "$TOOL_DIR" >> "$GITHUB_ENV"
fi

info "qemu: $(command -v qemu-system-aarch64) / $(command -v qemu-system-riscv64) / $(command -v qemu-system-x86_64)"
info "toolchains: $TOOL_DIR"
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    info "KVM available (x86 VMM/guest tests can run)"
else
    info "KVM unavailable: x86 VMM/guest tests will SKIP on this runner"
fi
