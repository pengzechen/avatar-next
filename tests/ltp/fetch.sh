#!/bin/bash
# fetch.sh — 拉取 LTP 源码到 tests/ltp/src
#
# 用法: bash tests/ltp/fetch.sh [dest] [version]
#       make env                # 等价入口（根目录或 tests/）
#
# 只下载源码；编译选定的测例仍由 tests/ltp/build.sh 负责：
#       make -C tests ltp-build
#
# 版本固定为 VERSION（默认 20260529，与 tests/ltp/testcases.list 验证过的
# 版本一致）。GitHub archive 的目录名是 ltp-<version>。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
DEST="${1:-$SCRIPT_DIR/src}"
VERSION="${2:-20260529}"
URL="https://github.com/linux-test-project/ltp/archive/refs/tags/${VERSION}.tar.gz"

info() { printf '\033[1;34m[ltp-env]\033[0m %s\n' "$*"; }
err()  { printf '\033[1;31m[ltp-env]\033[0m %s\n' "$*" >&2; }

# 已经是指定版本：幂等退出。
if [ -f "$DEST/VERSION" ] && [ "$(cat "$DEST/VERSION")" = "$VERSION" ]; then
    info "LTP $VERSION already present: $DEST"
    exit 0
fi

# 本地旧路径迁移（避免已经有一份 434MB 源码的人重新下载）。
if [ ! -d "$DEST" ] && [ -d "$ROOT_DIR/third_party/ltp" ]; then
    info "moving legacy third_party/ltp → $DEST"
    mkdir -p "$(dirname "$DEST")"
    mv "$ROOT_DIR/third_party/ltp" "$DEST"
    if [ "$(cat "$DEST/VERSION" 2>/dev/null || true)" = "$VERSION" ]; then
        info "LTP $VERSION ready: $DEST"
        exit 0
    fi
    err "legacy source version is $(cat "$DEST/VERSION" 2>/dev/null || echo unknown), expected $VERSION"
    err "remove $DEST and rerun, or set LTP_VERSION to that version"
    exit 1
fi

if [ -d "$DEST" ] && [ -f "$DEST/VERSION" ] &&
   [ "$(cat "$DEST/VERSION")" != "$VERSION" ]; then
    err "existing LTP source is version $(cat "$DEST/VERSION"), requested $VERSION"
    err "remove $DEST (or set LTP_VERSION=$(cat "$DEST/VERSION")) and retry"
    exit 1
fi

command -v curl >/dev/null 2>&1 || { err "curl not found"; exit 1; }
command -v tar  >/dev/null 2>&1 || { err "tar not found"; exit 1; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

info "downloading LTP $VERSION ..."
curl -fSL --retry 3 "$URL" -o "$tmp/ltp.tar.gz"
info "extracting ..."
tar -xzf "$tmp/ltp.tar.gz" -C "$tmp"

src="$tmp/ltp-$VERSION"
if [ ! -d "$src" ]; then
    # 某些 tag 的归档目录名可能不同，兜底找唯一的 ltp-* 目录。
    src="$(find "$tmp" -mindepth 1 -maxdepth 1 -type d -name 'ltp-*' | head -1)"
fi
[ -d "$src" ] || { err "cannot find extracted ltp-* directory"; exit 1; }

mkdir -p "$(dirname "$DEST")"
rm -rf "$DEST"
mv "$src" "$DEST"

if [ "$(cat "$DEST/VERSION" 2>/dev/null || true)" != "$VERSION" ]; then
    err "downloaded source has unexpected VERSION: $(cat "$DEST/VERSION" 2>/dev/null || echo unknown)"
    exit 1
fi

info "LTP $VERSION ready: $DEST"
info "next: make -C tests ltp-build"
