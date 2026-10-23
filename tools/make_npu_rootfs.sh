#!/usr/bin/env bash
#
# make_npu_rootfs.sh — 产出「宿主能跑 YOLO、guest 也能跑 YOLO」的 rootfs 镜像
#
# 它按固定顺序做三段（每段都可以单独跑，见各段开头的注释）：
#
#   §1 guest initrd  → imgs/guests/aarch64/initrd.gz
#                      装上 rknpu.ko + 一整套 glibc + librknnrt + yolov8n.rknn
#   §2 宿主 rootfs   → imgs/rootfs-aarch64.img
#                      = 基镜像 + /usr/lib/rknn{,-yolo} + /bin/{rknn,yolo} +
#                        /guests/linux/*（guest 内核/DTB/initrd）+ /bin/vmm-run
#
# ⚠️ **guest 内核不在仓库里**，得先把带 NPU 的 Image 编出来（见
#    docs/vmm/AARCH64_GUEST_NPU.md）。脚本按下面的顺序找它：
#       $GUEST_IMAGE  →  ~/kbuild/linux-guest-arm64/arch/arm64/boot/Image
#    找不到就**只跳过内核**、其余照做（并打一条显眼提示）—— 因为其余部分
#    （宿主侧 NPU、guest 驱动、initrd）都能独立验证，不该被一个路径拖住。
#
# 全程**不需要 sudo**：debugfs rdump 解包 + mkfs.ext4 -d 重打包。
#
# 用法：
#     tools/make_npu_rootfs.sh [基镜像] [输出镜像]
#
# 默认**就地覆盖** imgs/rootfs-aarch64.img —— 那正是真机上要烧的那个文件，
# 覆盖它意味着"下一次烧卡就是带 NPU 的那一版"。改坏了大不了
# `git checkout imgs/rootfs-aarch64.img` 退回（基镜像是入库的）。
# BASE == OUT 是**故意支持**的：§2 先把 BASE 整个解到临时目录，§3 才 rm OUT，
# 顺序上不会边读边删。
#
# 尺寸取平台配置的 rootfs 区大小（rk3588 是 128 MiB，见
# platforms/rk3588-aarch64/platform.conf）—— 超过它真机装不下。
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BASE=${1:-$ROOT/imgs/rootfs-aarch64.img}
OUT=${2:-$ROOT/imgs/rootfs-aarch64.img}
SIZE_MB=${SIZE_MB:-128}
RKNN=$ROOT/apps/rknn-aarch64
BUILD=${BUILD:-$ROOT/build/rk3588-aarch64}
GUESTS=$ROOT/imgs/guests/aarch64
# guest 内核（外部构建产物）。带 NPU 的那一版由项目外的内核树编出来。
GUEST_IMAGE=${GUEST_IMAGE:-$HOME/kbuild/linux-guest-arm64/arch/arm64/boot/Image}
GUEST_KO=${GUEST_KO:-$HOME/kbuild/rknpu-mod/rknpu-0.9.8/rknpu.ko}
# guest 的 NPU DTB 在仓库里（4.6 KB，值得入库 —— 它是手写的源文件）
GUEST_DTB=$GUESTS/linux-gicv3-npu.dtb

say() { printf '\033[32m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[33m警告:\033[0m %s\n' "$*" >&2; }
die() {
    printf '\033[31m错误:\033[0m %s\n' "$*" >&2
    exit 1
}

[ -f "$BASE" ] || die "缺基镜像 $BASE"
[ -d "$RKNN/lib" ] || die "缺 $RKNN/lib（见 apps/rknn-aarch64/README.md）"
[ -f "$GUEST_DTB" ] || die "缺 $GUEST_DTB"
[ -f "$GUESTS/linux.bin" ] || die "缺 $GUESTS/linux.bin"

# ── YOLOv8 启动包装 ──────────────────────────────────────────────────────
#
# 宿主和 guest **共用同一份**（路径全是绝对的，两边布局一样），所以写成一个
# 函数而不是复制两段 heredoc —— 两处不一致的话，症状是"宿主能跑、guest 不能"
# 或者反过来，而且差异藏在几十行 shell 里。
#
#   yolo [核数] [模型.rknn] [图片]
#
# 第一个参数只要是个纯数字就当成**核数**，其余按位置补齐默认值：
#
#   yolo                 # librknnrt 默认（AUTO＝单核），自带模型+图片
#   yolo 7               # 三核
#   yolo 1               # 只 core0
#   yolo 7 /tmp/m.rknn /tmp/x.png
#
# 检测结果直接打到 stdout；带框的图写到 /tmp/yolo/out.png。
#
# ⚠️ 为什么要用位置参数而不是环境变量：**本内核的 execve 不传环境变量**
#    （kernel/syscall/core/proc_lifecycle.c 里 `(void)envp;`），所以
#    `RKNN_CORE_MASK=7 yolo` 里的变量根本到不了 demo，getenv() 永远是 NULL。
#    argv 是可靠的。（guest 侧是正常 Linux，这条限制只对宿主成立。）
#
# 注意图片格式：demo 把 .jpg/.jpeg 交给 turbojpeg（本项目没编那个依赖），
# 其它扩展名才走内置的 stb_image —— 所以**请用 .png / .bmp**。
write_yolo_wrapper() {
    cat >"$1" <<'EOF'
#!/bin/sh
W=/tmp/yolo
mkdir -p "$W/model" 2>/dev/null
# ⚠️ 必须用 cp，**不能用 ln -s**：宿主内核只实现了「读/跟随」符号链接
#    （kernel/syscall/fs/path.c 的 follow_symlinks），**没有 symlink() 这个
#    系统调用** —— ln -s 会返回 ENOSYS 静默失败。
#    症状极具误导性：demo 一路正常跑完、框也画对，只是类别名全打成 "null"
#    （读不到 ./model/coco_80_labels_list.txt），而它唯一的提示是启动时一行
#    `Open ./model/coco_80_labels_list.txt fail!`。
#    （guest 是真 Linux，这条不适用 —— 但两边用同一份脚本，就不分开写了。）
cp /usr/lib/rknn-yolo/model/coco_80_labels_list.txt "$W/model/" 2>/dev/null
cd "$W" 2>/dev/null || cd /usr/lib/rknn-yolo || exit 1

# 第一个参数是纯数字就当核数（librknnrt 的 rknn_core_mask：1/3/7），
# 剩下的按位置当模型和图片。
case "$1" in
    1|2|3|4|5|6|7) MASK=$1; shift ;;
    *)             MASK= ;;
esac
MODEL=${1:-/usr/lib/rknn-yolo/yolov8n.rknn}
IMAGE=${2:-/usr/lib/rknn-yolo/bus.png}

if [ -n "$MASK" ]; then
    exec /usr/lib/rknn-yolo/ld-linux-aarch64.so.1 --library-path /usr/lib/rknn-yolo \
        /usr/lib/rknn-yolo/rknn_yolov8_demo "$MODEL" "$IMAGE" "$MASK"
fi
exec /usr/lib/rknn-yolo/ld-linux-aarch64.so.1 --library-path /usr/lib/rknn-yolo \
    /usr/lib/rknn-yolo/rknn_yolov8_demo "$MODEL" "$IMAGE"
EOF
    chmod 0755 "$1"
}

# ── §1  guest initrd ─────────────────────────────────────────────────────
#
# guest 的 rootfs 就是 initramfs，所以要在**这一个包里**同时放下：
#   /lib/modules/rknpu.ko    NPU 驱动（由 imgs/guests/initrd-common/init insmod）
#   /usr/lib/rknn-yolo/*     demo + 自带 glibc 运行时
#   /bin/yolo                启动包装
#
# ⚠️ initramfs 大小有硬上限： GUEST_LINUX_INITRD_LIMIT - GUEST_LINUX_INITRD_GPA
#    = **16 MiB**（见 include/guest_loader.h：上面是 NPU 恒等窗口，下面是内核
#    自己挑的 CMA，两头都占着）。超了会在加载**前**被拒（guest 起不来），
#    而不是悄悄截断 —— 这个包现在 9.2 MiB，别往里塞大件。
#
# ⚠️ **这里用 --gzip，另外两个架构的包是裸 cpio** —— 有意不一致。
#    tools/guest-initrd.py 的默认策略是"三个包都裸 cpio"，理由是省掉 guest 侧
#    的解压。但 aarch64 这个包因为要带 NPU 运行时，裸的可有 21 MiB，压完约
#    8 MiB：省下的 13 MiB 既是宿主 rootfs 的余量（那个区只有 128 MiB，
#    裸包会让镜像用掉 126/128），也是 guest RAM 的余量（192 MiB 里已经
#    挖走 48 MiB 给 NPU）。guest 内核 CONFIG_RD_GZIP=y，解压是它本来就支持的。
say "§1 造 guest initrd（rknpu.ko + YOLO 运行时）"
GTREE=$(mktemp -d /tmp/guest-npu-tree.XXXXXX)
STAGE=
# 用 ${VAR:+/...} 而不是 ${VAR:-}：STAGE 在 §2 才建，之前是空串，
# 直接 rm -rf "" 会返回 1 —— 在 EXIT trap 里那会把整个脚本的退出码改掉。
cleanup() { rm -rf "$GTREE" ${STAGE:+"$STAGE"}; }
trap cleanup EXIT

if [ -f "$GUEST_KO" ]; then
    mkdir -p "$GTREE/lib/modules"
    cp "$GUEST_KO" "$GTREE/lib/modules/rknpu.ko"
    say "  rknpu.ko  ($(stat -c%s "$GUEST_KO") 字节)"
else
    warn "找不到 $GUEST_KO —— guest initrd 里不会有 NPU 驱动"
    warn "  先按 docs/vmm/AARCH64_GUEST_NPU.md §3 把它编出来"
fi

if [ -d "$RKNN/yolo" ] && [ -f "$RKNN/yolo/rknn_yolov8_demo" ]; then
    mkdir -p "$GTREE/usr/lib/rknn-yolo" "$GTREE/bin"
    cp -a "$RKNN/yolo/." "$GTREE/usr/lib/rknn-yolo/"
    # 复用 rknn/ 那份 librknnrt（同一个文件，不重复收进仓库）
    cp "$RKNN/lib/librknnrt.so" "$GTREE/usr/lib/rknn-yolo/"
    chmod 0755 "$GTREE/usr/lib/rknn-yolo/ld-linux-aarch64.so.1" \
               "$GTREE/usr/lib/rknn-yolo/rknn_yolov8_demo"
    write_yolo_wrapper "$GTREE/bin/yolo"
    say "  yolo 运行时（$(du -sh "$GTREE/usr/lib/rknn-yolo" | cut -f1)）"
else
    warn "跳过 YOLO（$RKNN/yolo/ 不存在）—— guest 里没有可跑的东西"
fi

python3 "$ROOT/tools/guest-initrd.py" aarch64 --gzip --extra "$GTREE"
say "  → $GUESTS/initrd.gz ($(stat -c%s "$GUESTS/initrd.gz") 字节)"

# ── §2  宿主 rootfs ──────────────────────────────────────────────────────
STAGE=$(mktemp -d /tmp/npu-rootfs.XXXXXX)

say "§2 解包 $BASE → $STAGE"
# rdump 会抱怨 chown 失败（非 root），无害；文件内容与权限位都保住了。
# 基镜像里的文件是 uid 1000，当前用户也是 1000，所以重打包后属主不变。
debugfs -R "rdump / $STAGE" "$BASE" >/dev/null 2>&1 || true
[ -f "$STAGE/bin/busybox" ] || die "解包结果不对：没看到 /bin/busybox"
# lost+found 由 mkfs 自己建
rm -rf "$STAGE/lost+found"

# ── 2a. 宿主侧的 NPU 运行时 ───────────────────────────────────────────────
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
    cp "$RKNN/lib/librknnrt.so" "$STAGE/usr/lib/rknn-yolo/"
    chmod 0755 "$STAGE/usr/lib/rknn-yolo/ld-linux-aarch64.so.1" \
               "$STAGE/usr/lib/rknn-yolo/rknn_yolov8_demo"
    write_yolo_wrapper "$STAGE/bin/yolo"
    say "  rknn_yolov8_demo  ($(stat -c%s "$RKNN/yolo/rknn_yolov8_demo") 字节)"
    [ -f "$RKNN/yolo/yolov8n.rknn" ] && \
        say "  yolov8n.rknn      ($(stat -c%s "$RKNN/yolo/yolov8n.rknn") 字节)" || \
        say "  ⚠ 缺 yolov8n.rknn —— 转换那一步还没做"
else
    say "⚠ 跳过 YOLO（$RKNN/yolo/ 不存在或没有 demo 二进制）"
fi

# ── 2c. guest 镜像 + vmm-run（宿主 shell 里跑 guest 要用的东西）───────────
#
# 这一段是「在真机上从宿主 shell 起一个 Linux guest」的**全部**前提：
#   /guests/linux/linux.bin     guest 内核
#   /guests/linux/linux.dtb     guest DTB（必须与宿主 GIC 版本匹配）
#   /guests/linux/initrd.gz     guest rootfs
#   /bin/vmm-run                用户态 VMM helper，在宿主 shell 里敲它启动 guest
# 路径由 include/guest_loader.h 的 GUEST_LINUX_*_PATH 写死，改这里要一起改。
say "装 /guests/linux/ + /bin/vmm-run"
mkdir -p "$STAGE/guests/linux"

if [ -f "$GUEST_IMAGE" ]; then
    cp "$GUEST_IMAGE" "$STAGE/guests/linux/linux.bin"
    say "  linux.bin     ($(stat -c%s "$GUEST_IMAGE") 字节，带 NPU 驱动)"
else
    # 没有就用仓库里那份。它**没有** NPU 驱动，guest 里跑不了 yolo ——
    # 但至少 guest 能起来，宿主侧的东西也能验。
    cp "$GUESTS/linux.bin" "$STAGE/guests/linux/linux.bin"
    warn "找不到带 NPU 的 guest 内核（$GUEST_IMAGE）"
    warn "  退回仓库里那份 linux.bin —— guest 能启动，但**跑不了 NPU**"
fi

cp "$GUEST_DTB" "$STAGE/guests/linux/linux.dtb"
# 运行期 `vmm-run -c 1` 在“构建默认是多核”的内核上要用固定的单核 DTB。
# NPU DTB 本身就是单核，直接复用（构建默认是单核时 linux.dtb 也走它）。
cp "$GUEST_DTB" "$STAGE/guests/linux/linux-1cpu.dtb"
# 运行期 `vmm-run -c N`（N>1）用的 GICv3 多核 DTB。这些 DTB 没有 NPU 节点，
# 该 VM 不做 NPU 透传 —— 与 Makefile 里 GUEST_VCPUS>1 的限制一致。
for n in 2 4 8 12; do
    if [ -f "$GUESTS/linux-gicv3-smp$n.dtb" ]; then
        cp "$GUESTS/linux-gicv3-smp$n.dtb" "$STAGE/guests/linux/"
    fi
done
cp "$GUESTS/initrd.gz" "$STAGE/guests/linux/initrd.gz"
say "  linux.dtb     ($(stat -c%s "$GUEST_DTB") 字节，带 npu 节点)"
say "  linux-1cpu.dtb / linux-gicv3-smp{2,4,8,12}.dtb（运行期选核）"
say "  initrd.gz     ($(stat -c%s "$GUESTS/initrd.gz") 字节)"

if [ -f "$ROOT/apps/vmm-run-aarch64" ]; then
    mkdir -p "$STAGE/bin"
    cp "$ROOT/apps/vmm-run-aarch64" "$STAGE/bin/vmm-run"
    chmod 0755 "$STAGE/bin/vmm-run"
    say "  vmm-run       ($(stat -c%s "$ROOT/apps/vmm-run-aarch64") 字节)"
else
    die "缺 apps/vmm-run-aarch64 —— 先 make PLATFORM=rk3588-aarch64 apps/vmm-run-aarch64"
fi

# ── 2d. 本项目编的验收程序（musl 静态，0 个 NEEDED）──────────────────────
say "装 /bin/drmtest 与 /bin/dmatest"
for a in drmtest dmatest; do
    if [ -f "$BUILD/$a" ]; then
        cp "$BUILD/$a" "$STAGE/bin/$a"
        chmod 0755 "$STAGE/bin/$a"
        say "  $a  ($(stat -c%s "$BUILD/$a") 字节)"
    else
        say "  ⚠ 跳过 $a（$BUILD/$a 不存在 —— 先 make PLATFORM=rk3588-aarch64 NPU=rknpu $a）"
    fi
done

# ── §3  重打包 ───────────────────────────────────────────────────────────
say "§3 造 $OUT（${SIZE_MB} MiB）"
rm -f "$OUT"
dd if=/dev/zero of="$OUT" bs=1M count="$SIZE_MB" status=none
# 参数与 Makefile 的 rootfs 规则逐字一致 —— 那是内核 lwext4 验证过的组合。
mkfs.ext4 -q -b 1024 -L "avatarfs" -d "$STAGE" "$OUT"

USED=$((($(dumpe2fs -h "$OUT" 2>/dev/null | awk '/Block count/{print $3}') - \
    $(dumpe2fs -h "$OUT" 2>/dev/null | awk '/Free blocks/{print $3}')) * 1024 / 1048576))
say "完成：$OUT  (已用 ${USED} MiB / ${SIZE_MB} MiB)"
say "内容："
debugfs -R "ls -l /bin" "$OUT" 2>/dev/null | grep -E "rknn|yolo|drmtest|dmatest|vmm-run" | sed 's/^/    /'
debugfs -R "ls -l /guests/linux" "$OUT" 2>/dev/null | grep -v "^debugfs" | sed 's/^/    /'
