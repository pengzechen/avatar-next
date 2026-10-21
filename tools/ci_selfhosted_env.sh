#!/bin/bash
# ci_selfhosted_env.sh — self-hosted runner 的工具链 / KVM 自检
#
# 优先使用当前 PATH；如果工具链在常见的本地目录（用户机器上的
# Desktop/Software/compiler 布局），把它们追加到 $GITHUB_PATH。
set -euo pipefail

info() { printf '\033[1;34m[ci]\033[0m %s\n' "$*"; }
err()  { printf '\033[1;31m[ci]\033[0m %s\n' "$*" >&2; }

for base in \
    "${AVATAR_TOOLCHAIN_DIR:-}" \
    "$HOME/Desktop/Software/compiler" \
    "$HOME/SoftWare/compiler" \
    "$HOME/toolchains" \
    /opt/toolchains; do
    [ -n "$base" ] && [ -d "$base" ] || continue
    for d in "$base"/*-linux-musl-cross/bin "$base"/*/bin; do
        [ -d "$d" ] || continue
        if [ -n "${GITHUB_PATH:-}" ]; then
            printf '%s\n' "$d" >> "$GITHUB_PATH"
        fi
        export PATH="$d:$PATH"
    done
done

missing=0
for t in aarch64-linux-musl-gcc riscv64-linux-musl-gcc \
         qemu-system-aarch64 qemu-system-riscv64 qemu-system-x86_64 \
         mkfs.ext4 debugfs python3; do
    if command -v "$t" >/dev/null 2>&1; then
        info "$t: $(command -v "$t")"
    else
        err "missing: $t"
        missing=1
    fi
done
[ "$missing" -eq 0 ] || exit 1

if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    info "KVM available"
else
    info "KVM unavailable: x86 VMM/guest suites will SKIP (set AVATAR_TEST_NO_KVM=1 to silence)"
    export AVATAR_TEST_NO_KVM=1
    [ -n "${GITHUB_ENV:-}" ] && printf 'AVATAR_TEST_NO_KVM=1\n' >> "$GITHUB_ENV"
fi

if [ -d third_party/ltp ]; then
    info "LTP source present: third_party/ltp"
else
    info "LTP source missing: LTP suites will SKIP (tests/ltp/build.sh needs third_party/ltp)"
fi
if [ -d tests/ltp/bin/aarch64 ]; then
    info "LTP binaries present"
else
    info "LTP binaries missing: run 'make -C tests ltp-build' if source is available"
fi
