"""
rt-trace suite — RT_IRQ_TRACE 诊断构建的端到端验证。

这个 suite 专门检查“跟踪开关真的生效，且 /proc 能读到”：

  - 构建 RT_TEST=1 RT_DEMO=1 RT_IRQ_TRACE=1 变体；
  - 启动时 PREEMPTOFF TEST 必须 PASS（50µs 人工 preempt-off）；
  - 启动后从宿主 shell 读两次 /proc/rtstat；
  - 表头必须有 irq_off_*/preempt_off_* 列；
  - preempt_off_n > 0、preempt_off_max >= 人工持有的区间；
  - irq_off_n > 0、irq_off_max > 0、pc 非 0；
  - 执行 `ls /bin` 后计数只增不减（不重置统计）。

它不是性能阈值 suite：TRACE 版本有额外函数调用开销，绝对延迟只用于
确认计数/路径连通，不作为实时基线。性能看普通 rt-test suite。
"""

from __future__ import annotations

import re

from framework.context import TestContext
from framework.qemu import strip_ansi
from framework.report import Report

SUITE = "rt-trace"

PREEMPTOFF_RE = re.compile(
    rb"=== PREEMPTOFF TEST: PASS count=(\d+) held=(\d+) max=(\d+) ==="
)
RT_SCHED_PASS_RE = rb"=== RT SCHED TEST: PASS ==="
STATS_DONE1 = b"__RTSTAT_DONE1__"
STATS_DONE2 = b"__RTSTAT_DONE2__"
# 命令回显里不能出现完整的哨兵，否则 wait_for 会在命令真正执行前命中。
# 用 shell 变量拼接，回显是 "${x}DONE1__"，输出才是完整哨兵。
STATS_SET = b"__RTSTAT_"

FORBIDDEN = (
    rb"\[ASSERT\]",
    rb"\[ASSERT_ALWAYS\]",
    rb"Kernel panic",
    rb"panic:",
    rb"\[exception\] sync:",
    rb"=== BACKTRACE \(fault\)",
    rb"=== RT SCHED TEST: FAIL",
    rb"=== PREEMPTOFF TEST: FAIL",
    rb"=== OSLAT: FAIL",
    rb"=== CYCLICTEST: FAIL",
    rb"PMM: invalid free",
    rb"CORRUPT runqueue",
)


def _parse_stats(text: str):
    """返回 (header_tokens, [row_tokens...])。"""
    header = None
    rows = []
    for line in text.splitlines():
        parts = line.split()
        if parts and parts[0] == "cpu" and "preempt_off_max" in parts:
            header = parts
        elif parts and parts[0] == "cpu0":
            rows.append(parts)
    return header, rows


def _row_numbers(parts):
    if len(parts) != 22:
        raise AssertionError(f"rtstat cpu0 列数 {len(parts)}，应为 22：{parts}")
    vals = [int(parts[i]) for i in range(1, 14)]
    irq_off_max = int(parts[14])
    irq_off_last = int(parts[15])
    irq_off_n = int(parts[16])
    irq_pc = parts[17]
    preempt_max = int(parts[18])
    preempt_last = int(parts[19])
    preempt_n = int(parts[20])
    preempt_pc = parts[21]
    if not irq_pc.startswith("0x") or not preempt_pc.startswith("0x"):
        raise AssertionError(f"rtstat pc 不是十六进制：{parts}")
    return {
        "wake_max": vals[0],
        "wake_n": vals[2],
        "wake_min": vals[3],
        "wake_avg": vals[4],
        "syscall_max": vals[5],
        "syscall_avg": vals[8],
        "syscall_n": vals[9],
        "tick_gap_max": vals[10],
        "irq_off_max": irq_off_max,
        "irq_off_last": irq_off_last,
        "irq_off_n": irq_off_n,
        "irq_off_pc": irq_pc,
        "preempt_off_max": preempt_max,
        "preempt_off_last": preempt_last,
        "preempt_off_n": preempt_n,
        "preempt_off_pc": preempt_pc,
    }


def run(ctx: TestContext, report: Report, arches) -> None:
    for arch in arches:
        def one(arch=arch):
            ctx.ensure_build(arch, "rt-trace")
            q = ctx.qemu(
                SUITE,
                arch,
                "trace",
                variant="rt-trace",
                rootfs=True,
                kvm=ctx.kvm if arch.name == "x86_64" else None,
            )
            log = ctx.log_path(SUITE, arch, "trace")
            try:
                pre = q.wait_for(
                    PREEMPTOFF_RE,
                    ctx.timeout(240),
                    forbidden=FORBIDDEN,
                    desc=f"{arch.name} PREEMPTOFF 自测",
                )
                q.wait_for(
                    RT_SCHED_PASS_RE,
                    ctx.timeout(60),
                    forbidden=FORBIDDEN,
                    desc=f"{arch.name} RT 自测汇总",
                )
                q.wait_shell(ctx.timeout(240), desc=f"{arch.name} 宿主 shell")

                since = q.mark()
                q.send_slow(
                    b"x="
                    + STATS_SET
                    + b"; cat /proc/rtstat; echo ${x}DONE1__\n"
                )
                q.wait_for(
                    STATS_DONE1,
                    ctx.timeout(60),
                    since=since,
                    forbidden=FORBIDDEN,
                    desc=f"{arch.name} 第一次 rtstat",
                )

                since = q.mark()
                q.send_slow(
                    b"x="
                    + STATS_SET
                    + b"; ls /bin >/dev/null; cat /proc/rtstat; echo "
                    b"${x}DONE2__\n"
                )
                q.wait_for(
                    STATS_DONE2,
                    ctx.timeout(60),
                    since=since,
                    forbidden=FORBIDDEN,
                    desc=f"{arch.name} 第二次 rtstat",
                )
            finally:
                q.stop()

            text = strip_ansi(log.read_bytes()).decode("utf-8", "replace")
            header, rows = _parse_stats(text)
            assert header is not None, "没有找到带 preempt_off_max 的 rtstat 表头"
            for name in (
                "wake_min",
                "wake_avg",
                "syscall_avg",
                "irq_off_max",
                "irq_off_last",
                "irq_off_n",
                "irq_off_pc",
                "preempt_off_max",
                "preempt_off_last",
                "preempt_off_n",
                "preempt_off_pc",
            ):
                assert name in header, f"rtstat 表头缺列 {name}：{header}"

            assert len(rows) >= 2, f"只解析到 {len(rows)} 行 rtstat，应为 2 行"
            first = _row_numbers(rows[-2])
            second = _row_numbers(rows[-1])

            count, held, observed_max = (int(x) for x in pre.groups())
            assert count > 0, "PREEMPTOFF TEST 计数为 0"
            assert observed_max >= held, (
                f"PREEMPTOFF TEST max={observed_max} 小于 held={held}"
            )

            assert first["preempt_off_n"] > 0, "preempt_off_n 为 0（跟踪没生效？）"
            assert first["preempt_off_pc"] != "0x0", "preempt_off_pc 为 0"
            assert observed_max <= second["preempt_off_max"], (
                "PREEMPTOFF TEST 的 max 没有进入 /proc/rtstat"
            )
            assert second["preempt_off_n"] >= first["preempt_off_n"], (
                "执行 ls 后 preempt_off_n 变小了"
            )
            assert second["preempt_off_max"] >= first["preempt_off_max"], (
                "preempt_off_max 不是单调最大值"
            )

            assert first["wake_n"] > 0, "wake_n 为 0（wake 统计没生效？）"
            assert first["wake_min"] <= first["wake_avg"] <= first["wake_max"], (
                "wake min/avg/max 顺序错误："
                f"{first['wake_min']}/{first['wake_avg']}/{first['wake_max']}"
            )
            if first["syscall_n"] > 0:
                assert first["syscall_avg"] <= first["syscall_max"], (
                    "syscall avg > max："
                    f"{first['syscall_avg']}/{first['syscall_max']}"
                )

            assert first["irq_off_n"] > 0, "irq_off_n 为 0（跟踪没生效？）"
            assert first["irq_off_max"] > 0, "irq_off_max 为 0"
            assert first["irq_off_pc"] != "0x0", "irq_off_pc 为 0"
            assert second["irq_off_n"] >= first["irq_off_n"], (
                "执行 ls 后 irq_off_n 变小了"
            )
            assert second["irq_off_max"] >= first["irq_off_max"], (
                "irq_off_max 不是单调最大值"
            )

            tag = f"         {arch.name}: "
            print(
                f"{tag}preemptoff n={first['preempt_off_n']} "
                f"max={first['preempt_off_max']}ns "
                f"pc={first['preempt_off_pc']}"
            )
            print(
                f"{tag}irqoff n={first['irq_off_n']} "
                f"max={first['irq_off_max']}ns pc={first['irq_off_pc']}"
            )
            print(
                f"{tag}preemptoff self-test count={count} "
                f"held={held}ns max={observed_max}ns"
            )

        ctx.case(
            report,
            SUITE,
            arch,
            "trace",
            one,
            log=str(ctx.log_path(SUITE, arch, "trace")),
        )
