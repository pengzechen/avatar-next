"""
racetest suite — apps/c/racetest.c，SMP 竞争压力测试。

  racetest-slot  多进程 fork+wait，打任务槽回收/认领
  racetest-pipe  管道丢唤醒
  racetest-wake  两个 reader 并发 unblock 同一个 writer

需要 SMP>1；单核下内核侧不会被抢占，测不到竞争。二进制在框架的 normal
rootfs 的 /bin/racetest（根 Makefile 从 apps/c/racetest.c 构建）。

这是**诊断/压力** suite，不在 check-mm 的稳定门禁里：riscv64 SMP=4 上
`wake` 已经偶发触发 sched_schedule 的跑队列损坏（异常或挂死），在修掉
之前会让门禁间歇性变红。单独用 `make -C tests racetest SMP=4` 跑。
"""

from __future__ import annotations

from framework.context import SkipTest, TestContext
from framework.report import Report

SUITE = "racetest"

FORBIDDEN = (
    rb"\[ASSERT\]",
    rb"\[ASSERT_ALWAYS\]",
    rb"Kernel panic",
    rb"panic:",
    rb"Unable to handle",
    rb"PMM: invalid free",
    rb"CORRUPT runqueue",
    rb"\[exception\] sync:",
    rb"=== BACKTRACE \(fault\)",
)


def run(ctx: TestContext, report: Report, arches) -> None:
    for arch in arches:
        for args in ("slot 2 50", "pipe 50", "wake 50"):
            _one(ctx, report, arch, args)


def _one(ctx, report, arch, args: str) -> None:
    name = "racetest-" + args.split()[0]

    def one(arch=arch):
        if ctx.smp < 2:
            raise SkipTest("racetest 需要 SMP>1（单核下内核侧不可抢占）")
        ctx.ensure_build(arch, "normal")
        q = ctx.qemu(
            SUITE,
            arch,
            name,
            variant="normal",
            rootfs=True,
            kvm=ctx.kvm if arch.name == "x86_64" else None,
        )
        try:
            q.wait_shell(ctx.timeout(180), desc=f"{arch.name} 宿主 shell")
            since = q.mark()
            q.send_line(f"/bin/racetest {args}")
            q.wait_for(
                rb"DONE",
                ctx.timeout(240),
                since=since,
                forbidden=FORBIDDEN,
                desc=f"{arch.name} racetest {args}",
            )
        finally:
            q.stop()

    ctx.case(
        report, SUITE, arch, name, one, log=str(ctx.log_path(SUITE, arch, name))
    )
