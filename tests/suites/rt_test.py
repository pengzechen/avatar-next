"""
rt-test suite — RT 调度语义与唤醒/抢占延迟。

同一个 `rt-test` 变体同时编译两个启动自测（都不需要 rootfs）：

  - `RT_TEST=1`（tests/unit-test/rt_sched_test.c）
    场景 1–3：RT 优先级顺序 / FIFO 不被 tick 轮转；
    场景 4：NORMAL 忙循环唤醒 RT probe，断言 10 轮平均延迟 < 2ms
    （没有 self-IPI 立即抢占时该值≈tick 周期 5ms）；
    场景 5：模板 worker 与共享页——create/start/运行期参数/stop/destroy。
    有 oneshot 的架构（x86 KVM）按 1ms 实跑，200ms 内至少完成 3/4
    理论周期；早期自测窗口里 tick 回调尚未启用，aarch64/riscv 只验
    生命周期与共享页，不验周期频率。
    场景 6：mutex 并发压力（18000 次临界区、检测同时持锁）与 rt_mutex
    直接优先级继承（owner 提升到 80、等待者按 80/60 顺序拿锁、中等
    优先级任务不得插队、释放后恢复 0）。
    场景 7：rt_event any/all/mask 唤醒（含 x86 oneshot 超时）与 event
    模板 + 共享事件环：容量 64，先投 100 再处理（64 入环/36 drop），
    再补 20 条；最后验证阻塞在无限等事件的 worker 能被 stop 唤醒。
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
from framework.platforms import have_kvm
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
WORKER_RE = re.compile(
    rb"=== RT WORKER TEST: PASS period=(\d+) periodic=(\d+) ticks=(\d+) "
    rb"misses=(\d+) exec_max=(\d+) ==="
)
MUTEX_RE = re.compile(
    rb"=== RT MUTEX TEST: PASS mtx=(\d+) err=(\d+) boost=(\d+) "
    rb"after=(\d+) order=(\d+)/(\d+)/(\d+) ==="
)
EVENT_RE = re.compile(
    rb"=== RT EVENT TEST: PASS timeout=(\d+) posted=(\d+) "
    rb"processed=(\d+) dropped=(\d+) ==="
)

FORBIDDEN = (
    rb"\[ASSERT\]",
    rb"\[ASSERT_ALWAYS\]",
    rb"Kernel panic",
    rb"panic:",
    rb"\[exception\] sync:",
    rb"=== BACKTRACE \(fault\)",
    rb"=== RT SCHED TEST: FAIL",
    rb"=== RT WORKER TEST: FAIL",
    rb"=== RT MUTEX TEST: FAIL",
    rb"=== RT EVENT TEST: FAIL",
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

            worker = _parse(
                q.wait_for(
                    WORKER_RE,
                    ctx.timeout(180),
                    forbidden=FORBIDDEN,
                    desc=f"{arch.name} 模板 worker 自测",
                )
            )

            mutex = _parse(
                q.wait_for(
                    MUTEX_RE,
                    ctx.timeout(180),
                    forbidden=FORBIDDEN,
                    desc=f"{arch.name} mutex/rt_mutex 自测",
                )
            )

            event = _parse(
                q.wait_for(
                    EVENT_RE,
                    ctx.timeout(180),
                    forbidden=FORBIDDEN,
                    desc=f"{arch.name} rt_event/event 模板自测",
                )
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
        (
            worker_period,
            worker_periodic,
            worker_ticks,
            worker_misses,
            worker_exec_max,
        ) = worker
        (
            mutex_ops,
            mutex_err,
            mutex_boost,
            mutex_after,
            high_order,
            low_order,
            med_order,
        ) = mutex
        event_timeout, event_posted, event_processed, event_dropped = event

        print(
            f"         {arch.name}: ping-pong n={ping_n} "
            f"min/avg/max={ping_min}/{ping_avg}/{ping_max}ns; "
            f"preempt n={pre_n} min/avg/max={pre_min}/{pre_avg}/{pre_max}ns; "
            f"worker period={worker_period}ns periodic={worker_periodic} "
            f"ticks={worker_ticks} misses={worker_misses} "
            f"exec_max={worker_exec_max}ns; "
            f"mutex ops={mutex_ops} err={mutex_err} "
            f"pi={mutex_boost}->{mutex_after} order={high_order}/{low_order}/{med_order}; "
            f"event timeout={event_timeout} "
            f"posted/processed/dropped={event_posted}/{event_processed}/{event_dropped}"
        )

        assert mutex_ops == 6 * 3000, f"mutex 临界区计数 {mutex_ops}，应为 18000"
        assert mutex_err == 0, f"mutex 并发检查发现 {mutex_err} 次同时持锁"
        assert mutex_boost == 80, f"rt_mutex owner 提升到 {mutex_boost}，应为 80"
        assert mutex_after == 0, f"rt_mutex owner 释放后优先级 {mutex_after}，应为 0"
        assert (high_order, low_order, med_order) == (1, 2, 3), (
            "rt_mutex 等待者顺序 "
            f"{high_order}/{low_order}/{med_order}，应为 1/2/3"
            "（PI 失效时中等优先级任务会先完成）"
        )
        assert (event_posted, event_processed, event_dropped) == (84, 84, 36), (
            "event 环统计 "
            f"{event_posted}/{event_processed}/{event_dropped}，应为 84/84/36"
            "（容量 64，先投 100 再处理，之后补 20）"
        )
        if arch.name == "x86_64" and have_kvm():
            assert event_timeout == 1, "x86 KVM 下 rt_event 超时测试没有执行"
        else:
            assert event_timeout == 0, "没有 oneshot 时不应声称执行了超时测试"

        if arch.name == "x86_64" and have_kvm():
            assert worker_periodic == 1, "x86 KVM 下 worker 没有走 oneshot 周期"
            assert worker_period == 1_000_000, (
                f"x86 KVM 下 worker 周期是 {worker_period}ns，不是 1ms"
                "（TSC-deadline oneshot 失效？）"
            )
            assert worker_ticks > 0, "worker 没有完成任何周期"
        else:
            # 早期自测窗口里 tick 回调尚未启用，非 oneshot 架构不验周期。
            assert worker_periodic == 0, (
                f"{arch.name} 没有 oneshot，不应声称完成了周期"
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
