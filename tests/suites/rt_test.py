"""
rt-test suite — RT 调度语义与唤醒/抢占延迟。

同一个 `rt-test` 变体同时编译两个启动自测（都不需要 rootfs）：

  - `RT_TEST=1`（tests/unit-test/rt_sched_test.c）
    场景 1–3：RT 优先级顺序 / FIFO 不被 tick 轮转；
    场景 4：NORMAL 忙循环唤醒 RT probe，断言 10 轮平均延迟 < 2ms
    （没有 self-IPI 立即抢占时该值≈tick 周期 5ms）。
  - `RT_DEMO=1`（tests/unit-test/rt_demo.c）
    5000 轮 RT ping-pong + 50 轮 NORMAL→RT 抢占，输出 min/avg/max。

框架侧只做“症状门禁”：PASS 标记必须出现，demo 的样本数必须完整，
平均延迟不得超过上限。具体阈值见 docs/realtime/RT_DEMO_RESULTS.md。

判据适用于 KVM 和 TCG：无 self-IPI 时 NORMAL→RT 平均值由 guest 的
100Hz tick 决定（≈5ms），与宿主机快慢无关；self-IPI 路径在 TCG 下
实测也只有 ~20–120µs。
"""

from __future__ import annotations

import re

from framework.context import TestContext
from framework.report import Report

SUITE = "rt-test"

PING_ITERS = 5000
PREEMPT_ITERS = 50
# 有 self-IPI 时 x86 KVM ~2µs、TCG ~10–120µs；无 self-IPI 时≈5ms。
PING_AVG_MAX_NS = 1_000_000
PREEMPT_AVG_MAX_NS = 2_000_000

PING_RE = re.compile(
    rb"RT DEMO pingpong: n=(\d+) min=(\d+) avg=(\d+) max=(\d+) ns"
)
PREEMPT_RE = re.compile(
    rb"RT DEMO preempt: n=(\d+) min=(\d+) avg=(\d+) max=(\d+) ns"
)

FORBIDDEN = (
    rb"\[ASSERT\]",
    rb"\[ASSERT_ALWAYS\]",
    rb"Kernel panic",
    rb"panic:",
    rb"\[exception\] sync:",
    rb"=== BACKTRACE \(fault\)",
    rb"=== RT SCHED TEST: FAIL",
    rb"RT DEMO: TIMEOUT",
    rb"RT DEMO: worker/waker create failed",
    rb"RT DEMO: probe/hog create failed",
)


def run(ctx: TestContext, report: Report, arches) -> None:
    for arch in arches:
        _rt(ctx, report, arch)


def _parse(match: re.Match) -> tuple[int, ...]:
    return tuple(int(x) for x in match.groups())


def _rt(ctx: TestContext, report: Report, arch) -> None:
    def one(arch=arch):
        q = ctx.qemu(SUITE, arch, "rt", variant="rt-test", rootfs=False)
        try:
            q.wait_for(
                rb"=== RT SCHED TEST: PASS ===",
                ctx.timeout(180),
                forbidden=FORBIDDEN,
                desc=f"{arch.name} RT 调度自测",
            )

            ping = _parse(
                q.wait_for(
                    PING_RE,
                    ctx.timeout(180),
                    forbidden=FORBIDDEN,
                    desc=f"{arch.name} RT ping-pong 结果",
                )
            )
            pre = _parse(
                q.wait_for(
                    PREEMPT_RE,
                    ctx.timeout(180),
                    forbidden=FORBIDDEN,
                    desc=f"{arch.name} RT 抢占延迟结果",
                )
            )
        finally:
            q.stop()

        ping_n, ping_min, ping_avg, ping_max = ping
        pre_n, pre_min, pre_avg, pre_max = pre

        print(
            f"         {arch.name}: ping-pong n={ping_n} "
            f"min/avg/max={ping_min}/{ping_avg}/{ping_max}ns; "
            f"preempt n={pre_n} min/avg/max={pre_min}/{pre_avg}/{pre_max}ns"
        )

        assert ping_n == PING_ITERS, (
            f"ping-pong 只完成 {ping_n}/{PING_ITERS} 轮"
        )
        assert pre_n == PREEMPT_ITERS, (
            f"preempt 只完成 {pre_n}/{PREEMPT_ITERS} 轮"
        )
        assert ping_avg < PING_AVG_MAX_NS, (
            f"ping-pong 平均 {ping_avg}ns 超过 {PING_AVG_MAX_NS}ns"
        )
        assert pre_avg < PREEMPT_AVG_MAX_NS, (
            f"NORMAL→RT 抢占平均 {pre_avg}ns 超过 {PREEMPT_AVG_MAX_NS}ns"
            "（self-IPI 立即抢占失效？）"
        )

    ctx.case(
        report,
        SUITE,
        arch,
        "rt",
        one,
        log=str(ctx.log_path(SUITE, arch, "rt")),
    )
