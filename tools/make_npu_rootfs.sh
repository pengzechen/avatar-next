#!/usr/bin/env bash
#
# make_npu_rootfs.sh — 产出**带 NPU 运行时**的 rootfs 镜像
#
# 以 imgs/rootfs-aarch64.img 为**基**（真机现在用的那份），在上面加：
#
#   /usr/lib/rknn/      官方 glibc 运行时 + librknnrt.so + rknpu2 demo
#   /bin/rknn           启动包装（显式走自带 loader，绕开写死的 PT_INTERP）
#   /bin/drmtest        P2+P4 验收（本项目编的 musl 静态，0 个 NEEDED）
#   /bin/dmatest        P0 验收（同上）
#   /opt/rknn/include/  rknn_api.h / rknn_matmul_api.h
#
# ⚠️ 为什么 glibc 运行时装在 /usr/lib/rknn/ 而不是 /lib：
#    基镜像是 musl 的，/lib 里那些 libm.so.6 / libpthread.so.0 是**指向
#    musl libc.so 的符号链接**（只为让 glibc 风格的 DT_NEEDED 名字有落点）。
#    把真 glibc 的 libm.so.6 放进 /lib 会顶掉它们，反而弄坏所有 musl 程序。
#    详见 apps/rknn-aarch64/README.md。
#
# 全程**不需要 sudo**：debugfs rdump 解包 + mkfs.ext4 -d 重打包。
#
# 用法：
#     tools/make_npu_rootfs.sh [基镜像] [输出镜像]
#
# 默认基镜像 imgs/rootfs-aarch64.img，默认输出 imgs/rootfs-aarch64-npu.img。
# 尺寸取平台配置的 rootfs 区大小（rk3588 是 128 MiB，见
# platforms/rk3588-aarch64/platform.conf）—— 超过它真机装不下。
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BASE=${1:-$ROOT/imgs/rootfs-aarch64.img}
OUT=${2:-$ROOT/imgs/rootfs-aarch64-npu.img}
SIZE_MB=${SIZE_MB:-128}
RKNN=$ROOT/apps/rknn-aarch64
BUILD=${BUILD:-$ROOT/build/qemu-virt-aarch64}

say() { printf '\033[32m==>\033[0m %s\n' "$*"; }
die() {
    printf '\033[31m错误:\033[0m %s\n' "$*" >&2
    exit 1
}

[ -f "$BASE" ] || die "缺基镜像 $BASE"
[ -d "$RKNN/lib" ] || die "缺 $RKNN/lib（见 apps/rknn-aarch64/README.md）"

# ── 1. 解包基镜像 ────────────────────────────────────────────────────────
STAGE=$(mktemp -d /tmp/npu-rootfs.XXXXXX)
trap 'rm -rf "$STAGE"' EXIT

say "解包 $BASE → $STAGE"
# rdump 会抱怨 chown 失败（非 root），无害；文件内容与权限位都保住了。
# 基镜像里的文件是 uid 1000，当前用户也是 1000，所以重打包后属主不变。
debugfs -R "rdump / $STAGE" "$BASE" >/dev/null 2>&1 || true
[ -f "$STAGE/bin/busybox" ] || die "解包结果不对：没看到 /bin/busybox"
# lost+found 由 mkfs 自己建
rm -rf "$STAGE/lost+found"

# ── 2. NPU 运行时 ────────────────────────────────────────────────────────
say "装 /usr/lib/rknn/（glibc 运行时 + librknnrt + rknpu2）"
mkdir -p "$STAGE/usr/lib/rknn" "$STAGE/opt/rknn/include"
cp "$RKNN"/lib/* "$STAGE/usr/lib/rknn/"
cp "$RKNN/rknpu2" "$STAGE/usr/lib/rknn/rknpu2"
chmod 0755 "$STAGE/usr/lib/rknn/rknpu2"
chmod 0755 "$STAGE/usr/lib/rknn/ld-linux-aarch64.so.1"
cp "$RKNN"/include/* "$STAGE/opt/rknn/include/" 2>/dev/null || true

# 启动包装。用 #!/bin/sh（内核 elf_loader.c 支持 shebang）。
#
# 不直接跑 /usr/lib/rknn/rknpu2 的原因：它的 PT_INTERP 写死是
# /lib/ld-linux-aarch64.so.1，而那是 musl 的地盘（真 glibc loader 没放在
# 那儿）。显式用自带 loader 启动就完全绕开 PT_INTERP，也不需要动 /lib。
mkdir -p "$STAGE/bin"
cat >"$STAGE/bin/rknn" <<'EOF'
#!/bin/sh
# 跑官方 RKNPU demo（librknnrt）。
# 参数原样透传，所以模型路径之类的也能从命令行给。
exec /usr/lib/rknn/ld-linux-aarch64.so.1 --library-path /usr/lib/rknn \
    /usr/lib/rknn/rknpu2 "$@"
EOF
chmod 0755 "$STAGE/bin/rknn"

# ── 3. 本项目编的验收程序（musl 静态，0 个 NEEDED）────────────────────────
say "装 /bin/drmtest 与 /bin/dmatest"
for a in drmtest dmatest; do
    if [ -f "$BUILD/$a" ]; then
        cp "$BUILD/$a" "$STAGE/bin/$a"
        chmod 0755 "$STAGE/bin/$a"
        say "  $a  ($(stat -c%s "$BUILD/$a") 字节)"
    else
        say "  ⚠ 跳过 $a（$BUILD/$a 不存在 —— 先 make PLATFORM=qemu-virt-aarch64 NPU=rknpu $a）"
    fi
done

# ── 4. 重打包 ────────────────────────────────────────────────────────────
say "造 $OUT（${SIZE_MB} MiB）"
rm -f "$OUT"
dd if=/dev/zero of="$OUT" bs=1M count="$SIZE_MB" status=none
# 参数与 Makefile 的 rootfs 规则逐字一致 —— 那是内核 lwext4 验证过的组合。
mkfs.ext4 -q -b 1024 -L "avatarfs" -d "$STAGE" "$OUT"

USED=$((($(dumpe2fs -h "$OUT" 2>/dev/null | awk '/Block count/{print $3}') - \
    $(dumpe2fs -h "$OUT" 2>/dev/null | awk '/Free blocks/{print $3}')) * 1024 / 1048576))
say "完成：$OUT  (已用 ${USED} MiB / ${SIZE_MB} MiB)"
say "内容："
debugfs -R "ls -l /bin" "$OUT" 2>/dev/null | grep -E "rknn|drmtest|dmatest" | sed 's/^/    /'
debugfs -R "ls -l /usr/lib/rknn" "$OUT" 2>/dev/null | grep -vE "^debugfs|^\s+[0-9]+ +40755" | head -12 | sed 's/^/    /'
