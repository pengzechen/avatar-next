"""
boot suite — 三架构启动冒烟（不带 rootfs）。

判据：
  - 出现 PMM 的最终账目行（`PMM: N reserved regions, ... pages free`）；
  - 随后再观察一小段时间，不得出现 ASSERT / panic / invalid free。

没有 rootfs 是**故意的**：这一层只验证最早期的内存/调度/中断链路，
不把 ext4、busybox、LTP 这些上层因素混进来。文件系统挂载失败是预期输出。
"""

from __future__ import annotations

from framework.context import TestContext
from framework.report import Report

SUITE = "boot"

FORBIDDEN = (
    rb"\[ASSERT\]",
    rb"\[ASSERT_ALWAYS\]",
    rb"Kernel panic",
    rb"panic:",
    rb"Unable to handle",
    rb"PMM: invalid free",
    rb"PMM: double free",
    rb"refusing to free reserved",
    rb"\[exception\] sync:",
    rb"=== BACKTRACE \(fault\)",
)


def run(ctx: TestContext, report: Report, arches) -> None:
    for arch in arches:

        def one(arch=arch):
            q = ctx.qemu(
                SUITE,
                arch,
                f"boot-smp{ctx.smp}",
                variant="normal",
                rootfs=False,
                kvm=ctx.kvm if arch.name == "x86_64" else None,
            )
            try:
                q.wait_for(
                    [rb"PMM: \d+ reserved regions", rb"PMM: \d+ pages reserved"],
                    ctx.timeout(120),
                    forbidden=FORBIDDEN,
                    desc=f"{arch.name} 启动",
                )
                q.drain(ctx.timeout(2.0), forbidden=FORBIDDEN, desc=f"{arch.name} 启动后")
            finally:
                q.stop()

        ctx.case(
            report,
            SUITE,
            arch,
            f"boot-smp{ctx.smp}",
            one,
            log=str(ctx.log_path(SUITE, arch, f"boot-smp{ctx.smp}")),
        )
