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
    场景 5b：统计窗口——event worker 由 post 驱动，验证 reset 清空
    ticks/事件/共享页镜像但保留 out_seq；oneshot 下再验证 watchdog
    的 checks/trips/last_trip 以及 reset 后的清零。
    场景 6：mutex 并发压力（18000 次临界区、检测同时持锁）与 rt_mutex
    直接优先级继承（owner 提升到 80、等待者按 80/60 顺序拿锁、中等
    优先级任务不得插队、释放后恢复 0）。
    场景 7：rt_event any/all/mask 唤醒（含 x86 oneshot 超时）与 event
    模板 + 共享事件环：容量 64，先投 100 再处理（64 入环/36 drop），
    再补 20 条；最后验证阻塞在无限等事件的 worker 能被 stop 唤醒。
    另测 IPC 唤醒延迟：FIFO waiter 阻塞，FIFO waker set 后统计
    set→waiter 运行的 n/min/avg/p99/max（500 样本）。
    场景 8：内核态 cyclictest——FIFO 80 钉 CPU0，1ms(x86 oneshot)/10ms
    (tick 回退) 绝对 deadline 周期，加一个 NORMAL hog；记录
    min/avg/p50/p99/p99.9/max、stddev/jitter/drift、直方图、overrun
    ppm、早醒次数，以及 hog_iters（忙循环实际迭代数，>0 证明负载真的
    运行过）。x86 oneshot 下额外记录 timerlat IRQ 延迟（deadline →
    timer ISR 入口）。
    场景 9：内核态 oslat——FIFO 85 忙循环 500ms，记录相邻迭代间隔的
    min/avg/max 和超过 1/10/100µs 的次数，衡量已运行 RT 线程被 OS
    噪声打断的时间。
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
CYCLI_CYCLES = 2000
CYCLI_WARMUP = 100
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
OBS_RE = re.compile(
    rb"=== RT OBS TEST: PASS reset=(\d+) wd_tested=(\d+) "
    rb"wd_checks=(\d+) wd_trips=(\d+) ==="
)
MUTEX_RE = re.compile(
    rb"=== RT MUTEX TEST: PASS mtx=(\d+) err=(\d+) boost=(\d+) "
    rb"after=(\d+) order=(\d+)/(\d+)/(\d+) ==="
)
EVENT_RE = re.compile(
    rb"=== RT EVENT TEST: PASS timeout=(\d+) posted=(\d+) "
    rb"processed=(\d+) dropped=(\d+) ==="
)
IPC_RE = re.compile(
    rb"=== RT IPC TEST: PASS n=(\d+) min=(\d+) avg=(\d+) "
    rb"p99=(\d+) max=(\d+) ==="
)
CYCLI_RE = re.compile(
    rb"=== CYCLICTEST: PASS period=(\d+) warmup=(\d+) cycles=(\d+) "
    rb"samples=(\d+) min=(\d+) avg=(\d+) p50=(\d+) p99=(\d+) p999=(\d+) "
    rb"max=(\d+) stddev=(\d+) jitter=(\d+) drift=(-?\d+) "
    rb"overrun_ppm=(\d+) irq_n=(\d+) irq_min=(\d+) irq_avg=(\d+) "
    rb"irq_p99=(\d+) irq_max=(\d+) overruns=(\d+) early=(\d+) hog=(\d+) "
    rb"hog_iters=(\d+) hist=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+) ==="
)
OSLAT_RE = re.compile(
    rb"=== OSLAT: PASS duration=(\d+) samples=(\d+) min=(\d+) avg=(\d+) "
    rb"max=(\d+) over_1us=(\d+) over_10us=(\d+) over_100us=(\d+) ==="
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
    rb"=== RT OBS TEST: FAIL",
    rb"=== RT MIGRATION TEST: FAIL",
    rb"=== RT MUTEX TEST: FAIL",
    rb"=== RT EVENT TEST: FAIL",
    rb"=== RT IPC TEST: FAIL",
    rb"=== CYCLICTEST: FAIL",
    rb"=== OSLAT: FAIL",
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

            obs = _parse(
                q.wait_for(
                    OBS_RE,
                    ctx.timeout(180),
                    forbidden=FORBIDDEN,
                    desc=f"{arch.name} RT 统计窗口自测",
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

            ipc = _parse(
                q.wait_for(
                    IPC_RE,
                    ctx.timeout(180),
                    forbidden=FORBIDDEN,
                    desc=f"{arch.name} IPC 唤醒延迟自测",
                )
            )

            cycli = _parse(
                q.wait_for(
                    CYCLI_RE,
                    ctx.timeout(180),
                    forbidden=FORBIDDEN,
                    desc=f"{arch.name} cyclictest 结果",
                )
            )

            oslat = _parse(
                q.wait_for(
                    OSLAT_RE,
                    ctx.timeout(180),
                    forbidden=FORBIDDEN,
                    desc=f"{arch.name} oslat 结果",
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
        obs_reset, obs_wd_tested, obs_wd_checks, obs_wd_trips = obs
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
        ipc_n, ipc_min, ipc_avg, ipc_p99, ipc_max = ipc
        (
            cyc_period,
            cyc_warmup,
            cyc_cycles,
            cyc_samples,
            cyc_min,
            cyc_avg,
            cyc_p50,
            cyc_p99,
            cyc_p999,
            cyc_max,
            cyc_stddev,
            cyc_jitter,
            cyc_drift,
            cyc_overrun_ppm,
            cyc_irq_n,
            cyc_irq_min,
            cyc_irq_avg,
            cyc_irq_p99,
            cyc_irq_max,
            cyc_overruns,
            cyc_early,
            cyc_hog,
            cyc_hog_iters,
            *cyc_hist,
        ) = cycli
        (
            os_duration,
            os_samples,
            os_min,
            os_avg,
            os_max,
            os_over_1us,
            os_over_10us,
            os_over_100us,
        ) = oslat

        tag = f"         {arch.name}: "
        print(
            f"{tag}ping-pong n={ping_n} "
            f"min/avg/max={ping_min}/{ping_avg}/{ping_max}ns"
        )
        print(
            f"{tag}preempt n={pre_n} "
            f"min/avg/max={pre_min}/{pre_avg}/{pre_max}ns"
        )
        print(
            f"{tag}worker period={worker_period}ns periodic={worker_periodic}"
        )
        print(
            f"{tag}worker ticks={worker_ticks} misses={worker_misses} "
            f"exec_max={worker_exec_max}ns"
        )
        print(
            f"{tag}obs reset={obs_reset} wd_tested={obs_wd_tested} "
            f"wd_checks={obs_wd_checks} wd_trips={obs_wd_trips}"
        )
        print(
            f"{tag}mutex ops={mutex_ops} err={mutex_err} "
            f"pi={mutex_boost}->{mutex_after} "
            f"order={high_order}/{low_order}/{med_order}"
        )
        print(
            f"{tag}event timeout={event_timeout} "
            f"posted/processed/dropped="
            f"{event_posted}/{event_processed}/{event_dropped}"
        )
        print(
            f"{tag}ipc n={ipc_n} min/avg/p99/max="
            f"{ipc_min}/{ipc_avg}/{ipc_p99}/{ipc_max}ns"
        )
        print(
            f"{tag}cyclictest period={cyc_period}ns warmup={cyc_warmup} "
            f"cycles={cyc_cycles} samples={cyc_samples}"
        )
        print(
            f"{tag}cyclictest latency min/avg/p50="
            f"{cyc_min}/{cyc_avg}/{cyc_p50}ns"
        )
        print(
            f"{tag}cyclictest latency p99/p999/max="
            f"{cyc_p99}/{cyc_p999}/{cyc_max}ns"
        )
        print(
            f"{tag}cyclictest stddev/jitter="
            f"{cyc_stddev}/{cyc_jitter}ns drift={cyc_drift}ns "
            f"overrun_ppm={cyc_overrun_ppm}"
        )
        print(
            f"{tag}cyclictest irq_n={cyc_irq_n} "
            f"min/avg/p99/max="
            f"{cyc_irq_min}/{cyc_irq_avg}/{cyc_irq_p99}/{cyc_irq_max}ns"
        )
        print(f"{tag}cyclictest hist={cyc_hist}")
        print(
            f"{tag}cyclictest overruns={cyc_overruns} early={cyc_early} "
            f"hog={cyc_hog} hog_iters={cyc_hog_iters}"
        )
        print(
            f"{tag}oslat duration={os_duration}ns samples={os_samples}"
        )
        print(
            f"{tag}oslat min/avg/max={os_min}/{os_avg}/{os_max}ns "
            f"over_1/10/100us="
            f"{os_over_1us}/{os_over_10us}/{os_over_100us}"
        )

        assert obs_reset == 1, "RT 统计窗口 reset 没有通过"
        if arch.name == "x86_64" and have_kvm():
            assert obs_wd_tested == 1, "x86 KVM 下 watchdog 统计没有实际测试"
        if obs_wd_tested:
            assert obs_wd_checks >= 3, (
                f"watchdog 只检查了 {obs_wd_checks} 次，至少应为 3"
            )
            assert obs_wd_trips >= 1, "watchdog 目标不推进却没有 trip"
        else:
            assert obs_wd_checks == 0 and obs_wd_trips == 0, (
                "没有测试 watchdog 时不应有 checks/trips 计数"
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
        assert ipc_n == 500, f"IPC 唤醒样本 {ipc_n}，应为 500"
        assert ipc_min <= ipc_avg <= ipc_max, (
            "IPC min/avg/max 顺序错误："
            f"{ipc_min}/{ipc_avg}/{ipc_max}"
        )
        assert ipc_p99 <= ipc_max, (
            f"IPC p99={ipc_p99}ns 大于 max={ipc_max}ns"
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

        assert cyc_warmup == CYCLI_WARMUP, (
            f"cyclictest warmup={cyc_warmup}，应为 {CYCLI_WARMUP}"
        )
        assert cyc_cycles == CYCLI_CYCLES, (
            f"cyclictest 只完成 {cyc_cycles}/{CYCLI_CYCLES} 轮"
        )
        assert cyc_samples == CYCLI_CYCLES, (
            f"cyclictest 样本数 {cyc_samples}，应为 {CYCLI_CYCLES}"
        )
        assert cyc_hog == 1, "cyclictest 没有运行 NORMAL hog 负载"
        assert cyc_hog_iters > 0, (
            "cyclictest 的 NORMAL hog 没有获得任何 CPU 时间（hog_iters=0）"
        )
        assert os_duration == 500_000_000, (
            f"oslat duration={os_duration}ns，应为 500ms"
        )
        assert os_samples > 0, "oslat RT 线程没有完成任何循环"
        assert os_min <= os_avg <= os_max, (
            "oslat min/avg/max 顺序错误："
            f"{os_min}/{os_avg}/{os_max}"
        )
        assert os_over_1us >= os_over_10us >= os_over_100us, (
            "oslat 超阈值计数顺序错误："
            f"{os_over_1us}/{os_over_10us}/{os_over_100us}"
        )

        assert sum(cyc_hist) == cyc_samples, (
            f"cyclictest 直方图合计 {sum(cyc_hist)}，应为 {cyc_samples}"
        )
        assert cyc_stddev <= cyc_jitter, (
            f"cyclictest stddev={cyc_stddev}ns 大于 jitter={cyc_jitter}ns"
        )
        assert cyc_overrun_ppm == (cyc_overruns * 1_000_000 // cyc_samples), (
            "cyclictest overrun_ppm 与 overruns/samples 不一致："
            f"{cyc_overrun_ppm} vs {cyc_overruns}/{cyc_samples}"
        )

        assert cyc_irq_n <= cyc_samples, (
            f"IRQ timer 样本 {cyc_irq_n} 多于总样本 {cyc_samples}"
        )
        if cyc_period == 1_000_000:
            assert cyc_irq_n == cyc_samples, (
                f"oneshot 下只记录到 {cyc_irq_n}/{cyc_samples} 个 IRQ timer 样本"
            )
            assert cyc_irq_min <= cyc_irq_avg <= cyc_irq_max, (
                "IRQ timer min/avg/max 顺序错误："
                f"{cyc_irq_min}/{cyc_irq_avg}/{cyc_irq_max}"
            )
            assert cyc_irq_max <= cyc_max, (
                f"IRQ max={cyc_irq_max}ns 大于总唤醒 max={cyc_max}ns"
                "（deadline→ISR 不可能晚于 deadline→线程）"
            )
            assert cyc_irq_max < 5_000_000, (
                f"IRQ timer max={cyc_irq_max}ns 超过 5ms"
            )
        else:
            assert cyc_irq_n == 0, (
                "没有 oneshot 却记录了 IRQ timer 延迟"
            )

        if arch.name == "x86_64" and have_kvm():
            assert cyc_period == 1_000_000, (
                f"x86 KVM 下 cyclictest 周期是 {cyc_period}ns，不是 1ms"
                "（TSC-deadline oneshot 失效？）"
            )
            assert cyc_irq_p99 < 1_000_000, (
                f"x86 KVM 下 IRQ timer p99={cyc_irq_p99}ns 超过 1ms"
            )
            assert os_max < 5_000_000, (
                f"x86 KVM 下 oslat max={os_max}ns 超过 5ms"
            )
            # KVM 下 hrtimer 唤醒应在 µs 级；1ms p99 / 5ms max 是回归门禁，
            # 不是“软实时达标线”。真实硬件基线见 docs/realtime/CYCLICTEST.md。
            assert cyc_p99 < 1_000_000, (
                f"cyclictest p99={cyc_p99}ns 超过 1ms（x86 KVM）"
            )
            assert cyc_max < 5_000_000, (
                f"cyclictest max={cyc_max}ns 超过 5ms（x86 KVM）"
            )

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
