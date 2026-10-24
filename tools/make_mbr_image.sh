#!/bin/bash
# make_mbr_image.sh — 生成 p1 FAT + p2 ext4 的 MBR 磁盘镜像
#
# 用法: tools/make_mbr_image.sh <stage_dir> <out.img> <total_mb> <fat_mb>
#
# 布局（LBA 512B）：
#   p1: 从 1 MiB 开始，FAT32 (type 0x0c)，放 hello.txt / boot.cmd
#   p2: 紧跟 p1，ext4 (type 0x83)，内容来自 rootfs stage
#
# 需要 host 安装: dosfstools(mkfs.fat)、mtools(mcopy)、e2fsprogs、util-linux(sfdisk)。
set -euo pipefail

if [ "$#" -ne 4 ]; then
    echo "usage: $0 <stage_dir> <out.img> <total_mb> <fat_mb>" >&2
    exit 2
fi

STAGE=$1
OUT=$2
TOTAL_MB=$3
FAT_MB=$4

[ -d "$STAGE" ] || { echo "missing stage dir: $STAGE" >&2; exit 1; }
[ "$TOTAL_MB" -gt $((FAT_MB + 16)) ] || {
    echo "total_mb ($TOTAL_MB) must be at least fat_mb ($FAT_MB) + 16" >&2
    exit 1
}

EXT4_MB=$((TOTAL_MB - FAT_MB - 1))
FAT_START_MB=1
EXT4_START_MB=$((FAT_START_MB + FAT_MB))

TMP=$(mktemp -d)
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

FATIMG="$TMP/fat.img"
EXT4IMG="$TMP/ext4.img"
SEED="$TMP/seed"

echo "=== make_mbr_image: ${OUT} (${TOTAL_MB} MiB) ==="
echo "  p1 FAT   : start=${FAT_START_MB} MiB size=${FAT_MB} MiB"
echo "  p2 ext4  : start=${EXT4_START_MB} MiB size=${EXT4_MB} MiB"

# ── p1: FAT32 ────────────────────────────────────────────────────────────
mkdir -p "$SEED"
printf 'hello from FAT\n' > "$SEED/hello.txt"
printf '# boot script from FAT\n' > "$SEED/boot.cmd"

dd if=/dev/zero of="$FATIMG" bs=1M count="$FAT_MB" status=none
# FAT32 的最小卷约 33 MiB；小分区用 FAT16（两者 FatFs/U-Boot 都支持）。
# -h 2048: BPB 的 hidden sectors = p1 的起始 LBA（1 MiB / 512）
if [ "$FAT_MB" -lt 33 ]; then FAT_TYPE=16; else FAT_TYPE=32; fi
mkfs.fat -F "$FAT_TYPE" -h 2048 -n BOOT "$FATIMG" >/dev/null
mcopy -i "$FATIMG" -o "$SEED/hello.txt" "$SEED/boot.cmd" ::/

# ── p2: ext4 ─────────────────────────────────────────────────────────────
dd if=/dev/zero of="$EXT4IMG" bs=1M count="$EXT4_MB" status=none
mkfs.ext4 -q -b 1024 -L "avatarfs" -d "$STAGE" "$EXT4IMG"

# ── 磁盘 + MBR ───────────────────────────────────────────────────────────
dd if=/dev/zero of="$OUT" bs=1M count="$TOTAL_MB" status=none

FAT_SECTORS=$((FAT_MB * 2048))
EXT4_SECTORS=$((EXT4_MB * 2048))
{
    echo 'label: dos'
    echo 'unit: sectors'
    echo "start=$((FAT_START_MB * 2048)), size=${FAT_SECTORS}, type=c, bootable"
    echo "start=$((EXT4_START_MB * 2048)), size=${EXT4_SECTORS}, type=83"
} | sfdisk --quiet "$OUT"

dd if="$FATIMG" of="$OUT" bs=1M seek="$FAT_START_MB" conv=notrunc status=none
dd if="$EXT4IMG" of="$OUT" bs=1M seek="$EXT4_START_MB" conv=notrunc status=none

ACTUAL=$(stat -c '%s' "$OUT")
EXPECT=$((TOTAL_MB * 1024 * 1024))
[ "$ACTUAL" -eq "$EXPECT" ] || {
    echo "image size mismatch: $ACTUAL != $EXPECT" >&2
    exit 1
}

echo "=== MBR image ready: ${OUT} ==="
