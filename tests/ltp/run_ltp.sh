#!/bin/sh
# run_ltp.sh — 在目标板上批量运行 LTP 测例
#
# 用法: /ltp/run_ltp.sh
# 需要: busybox sh, 测例二进制在同目录下
#
# 退出码含义 (LTP 标准):
#   0  = TPASS
#   1  = TFAIL
#   2  = TBROK
#   4  = TWARN
#   32 = TCONF (跳过)

PASS=0
FAIL=0
SKIP=0
BROK=0
TOTAL=0

echo "=== Avatar OS LTP Test Suite ==="
echo ""

for test in /ltp/*; do
    name="${test##*/}"
    case "$name" in
        run_ltp.sh) continue ;;
        *.sh)       continue ;;
    esac
    [ -x "$test" ] || continue

    TOTAL=$((TOTAL + 1))
    echo "--- [$TOTAL] $name ---"
    "$test"
    ret=$?
    case $ret in
        0)  PASS=$((PASS + 1)) ;;
        32) SKIP=$((SKIP + 1)); echo "  => SKIPPED (TCONF)" ;;
        2)  BROK=$((BROK + 1)); echo "  => BROKEN" ;;
        *)  FAIL=$((FAIL + 1)); echo "  => FAILED (exit=$ret)" ;;
    esac
    echo ""
done

echo "======================================="
echo " LTP Results: $TOTAL tests"
echo "   PASS: $PASS"
echo "   FAIL: $FAIL"
echo "   SKIP: $SKIP"
echo "   BROK: $BROK"
echo "======================================="

if [ "$FAIL" -gt 0 ] || [ "$BROK" -gt 0 ]; then
    exit 1
fi
exit 0
