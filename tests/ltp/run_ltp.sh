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

# LTP 找辅助二进制（*_child）的顺序是：
#   $LTP_DATAROOT → $LTPROOT/testcases/bin → **测试启动时所在的目录**
#   （lib/tst_resource.c 的 tst_get_startwd 兜底）
# build.sh 已经把 <bin>_child 一起拷进了 /ltp，但 shell 的 CWD 是 /，
# 前两条路都不存在，于是报 "Failed to copy resource 'xxx_child'"。
# 把 CWD 切到 /ltp 即可命中第三条。
#   实测受影响：pipe2_02、getrusage03、openat02
cd /ltp || exit 1

# 测试框架用文件传递子集：宿主的串口控制台会把过长的
# `LTP_TESTS="..."` 命令行折行/丢字节，变量经常根本没生效。
# 每行一个测例名，框架用多条短 echo 写进来。
LTP_TESTS="${LTP_TESTS:-}"
if [ -z "$LTP_TESTS" ] && [ -f /tmp/ltp.list ]; then
    # 用 shell 内建逐行读，不依赖 tr/cat：busybox 里没有 tr，
    # 而且 case 的 " $名 " 匹配必须把换行拼成空格。
    while IFS= read -r line; do
        [ -n "$line" ] && LTP_TESTS="$LTP_TESTS $line"
    done < /tmp/ltp.list
fi

# 排除列表：每行一个测例名。框架跑 ltp-full 时把计时类慢用例
# （pselect01/poll02，各睡 8.25s）写进 /tmp/ltp.skip，避免日常全量被它们拖慢。
LTP_SKIP=""
if [ -f /tmp/ltp.skip ]; then
    while IFS= read -r line; do
        [ -n "$line" ] && LTP_SKIP="$LTP_SKIP $line"
    done < /tmp/ltp.skip
fi

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
        # 辅助二进制（build.sh 会跟着主测例一起拷进来，供主测例运行期 exec）。
        # 单独跑它们不带参数、没有意义，计成 FAIL 会污染结果。
        *_child)    continue ;;
    esac
    [ -x "$test" ] || continue

    # 子集模式：LTP_TESTS="brk01 mmap01" /ltp/run_ltp.sh
    # 空值 = 跑全部。用空格定界，避免子串误匹配（read01 vs read010）。
    if [ -n "${LTP_TESTS:-}" ]; then
        case " $LTP_TESTS " in
            *" $name "*) ;;
            *) continue ;;
        esac
    fi

    # 排除模式：ltp-full 用 /tmp/ltp.skip 跳过计时类慢用例。
    if [ -n "${LTP_SKIP:-}" ]; then
        case " $LTP_SKIP " in
            *" $name "*) continue ;;
        esac
    fi

    TOTAL=$((TOTAL + 1))
    echo "--- [$TOTAL] $name ---"
    "$test"
    ret=$?
    case $ret in
        0)  status=PASS; PASS=$((PASS + 1)) ;;
        32) status=SKIP; SKIP=$((SKIP + 1)); echo "  => SKIPPED (TCONF)" ;;
        2)  status=BROK; BROK=$((BROK + 1)); echo "  => BROKEN" ;;
        *)  status=FAIL; FAIL=$((FAIL + 1)); echo "  => FAILED (exit=$ret)" ;;
    esac
    # 测试框架的机器可读行；不要改格式（framework/ltp.py 依赖它）。
    echo "LTP-CASE: $name $status"
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
