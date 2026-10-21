"""
build suite — 三架构构建矩阵。

每个变体一个独立的 BUILD_ROOT（见 framework/platforms.py），所以这里可以
放心地按任意顺序构建，不会出现"后一个变体把前一个内核覆盖掉"。
"""

from __future__ import annotations

import subprocess

from framework import platforms
from framework.context import TestContext
from framework.report import Report

SUITE = "build"
DEFAULT_VARIANTS = ("normal", "guest", "vmm-test", "string-test", "pmm-test")


def _check_guest_tag(kernel) -> None:
    """guest 产物必须带构建指纹；否则说明被别的变体覆盖了。"""
    out = subprocess.run(
        ["strings", str(kernel)], capture_output=True, text=True
    ).stdout
    n = out.count("GUEST_LINUX_BUILD_TAG")
    assert n == 1, (
        f"{kernel} 的 GUEST_LINUX_BUILD_TAG 出现 {n} 次（应为 1）—— "
        "产物不是 guest 变体，或构建目录被污染"
    )


def run(
    ctx: TestContext,
    report: Report,
    arches,
    variants=None,
) -> None:
    variants = variants or DEFAULT_VARIANTS
    for arch in arches:
        for variant in variants:
            def one(arch=arch, variant=variant):
                ctx.ensure_build(arch, variant, force=True)
                if variant == "guest":
                    _check_guest_tag(
                        platforms.kernel_path(
                            ctx.artifacts, arch, "guest", ctx.smp
                        )
                    )

            ctx.case(
                report,
                SUITE,
                arch,
                variant,
                one,
                log=str(ctx.artifacts / "build-logs" / f"{arch.name}-{variant}.log"),
            )
