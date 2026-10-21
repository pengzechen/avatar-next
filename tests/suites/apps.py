"""
apps suite — 迁移 apps/c/pthread 与 apps/c/mutex。

  apps/pthread   /bin/pthread_test（pthread/futex/cond/TLS）
  apps/mutex     /bin/mutex_test（futex 自实现 mutex/rmutex）

这两个二进制只存在于 `apps/c/build.sh` 生成的 `imgs/rootfs-<arch>.img`
里（根 Makefile 的 rootfs 只装 apps/<arch>/*.c），所以直接用**预编译
rootfs** + 测试框架构建的当前内核。

竞争类测试（racetest）单独在 `suites/racetest.py`，因为它是 SMP 压力
诊断，可能与已知的调度器间歇性 bug 一起变红。
"""

from __future__ import annotations

import pathlib

from framework.context import SkipTest, TestContext
from framework.qemu import strip_ansi
from framework.report import Report

SUITE = "apps"
REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]

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


def _prebuilt_rootfs(arch) -> pathlib.Path:
    path = REPO_ROOT / "imgs" / f"rootfs-{arch.name}.img"
    if not path.is_file():
        raise SkipTest(
            f"缺少 {path}；先跑 bash apps/c/build.sh 生成三架构动态 rootfs"
        )
    return path


def _check_fail_lines(text: bytes, program: str) -> None:
    clean = strip_ansi(text).decode("utf-8", "replace")
    fails = [ln for ln in clean.splitlines() if "[FAIL]" in ln]
    passes = [ln for ln in clean.splitlines() if "[PASS]" in ln]
    assert not fails, f"{program} 有失败用例:\n" + "\n".join(fails[:20])
    assert passes, f"{program} 没有解析到任何 [PASS] 行（程序没跑起来？）"


def _run_prebuilt(ctx, report, arch, name: str, cmd: str) -> None:
    def one(arch=arch):
        rootfs = _prebuilt_rootfs(arch)
        ctx.ensure_build(arch, "normal")
        q = ctx.qemu(
            SUITE,
            arch,
            name,
            variant="normal",
            rootfs_file=rootfs,
            kvm=ctx.kvm if arch.name == "x86_64" else None,
        )
        log = ctx.log_path(SUITE, arch, name)
        try:
            q.wait_shell(ctx.timeout(180), desc=f"{arch.name} 宿主 shell")
            since = q.mark()
            q.send_line(cmd)
            q.wait_for(
                rb"=== Done ===",
                ctx.timeout(180),
                since=since,
                forbidden=FORBIDDEN,
                desc=f"{arch.name} {cmd}",
            )
        finally:
            q.stop()
        _check_fail_lines(log.read_bytes(), cmd)

    ctx.case(
        report, SUITE, arch, name, one, log=str(ctx.log_path(SUITE, arch, name))
    )


def run(ctx: TestContext, report: Report, arches) -> None:
    for arch in arches:
        _run_prebuilt(ctx, report, arch, "pthread", "/bin/pthread_test")
        _run_prebuilt(ctx, report, arch, "mutex", "/bin/mutex_test")
