#!/usr/bin/env bash
#
# make_npu_rootfs.sh — 产出**带 NPU 运行时**的 rootfs 镜像
#
# 以 imgs/rootfs-aarch64.img 为**基**（真机现在用的那份），在上面加：
#
#   /usr/lib/rknn/      官方 glibc 运行时 + librknnrt.so + rknpu2 demo
#   /bin/rknn           启动包装（显式走自带 loader，绕开写死的 PT_INTERP）
#   /usr/lib/rknn-yolo/ YOLOv8 demo + **另一套** glibc 运行时（见下）
#   /bin/yolo           启动包装（同上）
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
# ⚠️ 为什么 YOLO 要**再带一套** glibc（rknn-yolo/），而不是共用 rknn/：
#    两套的需求不同。rknn/ 里那份是上游发的运行时（GLIBC_2.30、GLIBCXX_3.4.28），
#    而 rknn_model_zoo 的 C++ demo 用 ubuntu 交叉工具链编出来，要
#    **GLIBC_2.34 + GLIBCXX_3.4.29** —— 老的那套满足不了，硬放进去是**加载即失败**。
#    反过来把新版塞进 rknn/ 又会动到已经在真机上验证过的那条路（rknpu2）。
#    所以各带一套、互不干扰：**rknpu2 那条路一个字节都没变**。
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

# ── 2b. YOLOv8 demo（rknn_model_zoo 的 C++ 例子）─────────────────────────
#
# 版本要求与 rknn/ 不同（GLIBC_2.34 / GLIBCXX_3.4.29），所以自带一套运行时。
# 产物怎么来的见 apps/rknn-aarch64/README.md「YOLOv8 demo」那一节。
if [ -d "$RKNN/yolo" ] && [ -f "$RKNN/yolo/rknn_yolov8_demo" ]; then
    say "装 /usr/lib/rknn-yolo/（YOLOv8 demo + 自带 glibc 运行时）"
    mkdir -p "$STAGE/usr/lib/rknn-yolo"
    cp -a "$RKNN/yolo/." "$STAGE/usr/lib/rknn-yolo/"
    # 复用 rknn/ 那份 librknnrt（同一个文件，不重复收进仓库）
    cp "$RKNN/lib/librknnrt.so" "$STAGE/usr/lib/rknn-yolo/"
    chmod 0755 "$STAGE/usr/lib/rknn-yolo/ld-linux-aarch64.so.1" \
               "$STAGE/usr/lib/rknn-yolo/rknn_yolov8_demo"

    # ⚠️ demo 里 LABEL_NALE_TXT_PATH 是**相对路径** "./model/coco_80_labels_list.txt"
    #    （postprocess.cc:26），结果图 out.png 也写在当前目录 —— 所以必须在
    #    "含 model/ 的可写目录" 下启动。直接 cd 到 /usr/lib/rknn-yolo 的话，
    #    若根文件系统只读，最后写 out.png 会失败（检测结果早就打完了，
    #    但退出码不好看）。改在 /tmp 下拼一个工作目录，两条都满足。
    cat >"$STAGE/bin/yolo" <<'EOF'
#!/bin/sh
# YOLOv8 目标检测 demo（rknn_model_zoo / rk3588）。
#
#   yolo [模型.rknn] [图片]
#
# 两个参数都可省，默认用自带的 yolov8n.rknn + bus.png。
# 检测结果直接打到 stdout；带框的图写到 /tmp/yolo/out.png。
# 换自己的图：先放进 rootfs（或 /tmp），再 yolo /usr/lib/rknn-yolo/yolov8n.rknn /tmp/你的图.png
#
# 注意图片格式：demo 把 .jpg/.jpeg 交给 turbojpeg（本项目没编那个依赖），
# 其它扩展名才走内置的 stb_image —— 所以**请用 .png / .bmp**。
W=/tmp/yolo
mkdir -p "$W/model" 2>/dev/null
# ⚠️ 必须用 cp，**不能用 ln -s**：本内核只实现了「读/跟随」符号链接
#    （kernel/syscall/fs/path.c 的 follow_symlinks），**没有 symlink() 这个
#    系统调用** —— ln -s 会返回 ENOSYS 静默失败。
#    症状极具误导性：demo 一路正常跑完、框也画对，只是类别名全打成 "null"
#    （读不到 ./model/coco_80_labels_list.txt），而它唯一的提示是启动时一行
#    `Open ./model/coco_80_labels_list.txt fail!`。
cp /usr/lib/rknn-yolo/model/coco_80_labels_list.txt "$W/model/" 2>/dev/null
cd "$W" 2>/dev/null || cd /usr/lib/rknn-yolo || exit 1
exec /usr/lib/rknn-yolo/ld-linux-aarch64.so.1 --library-path /usr/lib/rknn-yolo \
    /usr/lib/rknn-yolo/rknn_yolov8_demo \
    "${1:-/usr/lib/rknn-yolo/yolov8n.rknn}" \
    "${2:-/usr/lib/rknn-yolo/bus.png}"
EOF
    chmod 0755 "$STAGE/bin/yolo"
    say "  rknn_yolov8_demo  ($(stat -c%s "$RKNN/yolo/rknn_yolov8_demo") 字节)"
    [ -f "$RKNN/yolo/yolov8n.rknn" ] && \
        say "  yolov8n.rknn      ($(stat -c%s "$RKNN/yolo/yolov8n.rknn") 字节)" || \
        say "  ⚠ 缺 yolov8n.rknn —— 转换那一步还没做"
else
    say "⚠ 跳过 YOLO（$RKNN/yolo/ 不存在或没有 demo 二进制）"
fi

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
debugfs -R "ls -l /bin" "$OUT" 2>/dev/null | grep -E "rknn|yolo|drmtest|dmatest" | sed 's/^/    /'
debugfs -R "ls -l /usr/lib/rknn" "$OUT" 2>/dev/null | grep -vE "^debugfs|^\s+[0-9]+ +40755" | head -12 | sed 's/^/    /'
