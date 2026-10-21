"""
kernel suite — 编译进内核的自测（PMM / string / VMM 三线程）。

这些测试不需要 rootfs（VMM_TEST / PMM_TEST / STRING_TEST 都在 early boot
路径上），所以用独立的 BUILD_ROOT 构建后直接启动。
"""

from __future__ import annotations

import re

from framework.context import SkipTest, TestContext
from framework.report import Report

SUITE = "kernel"

FORBIDDEN = (
    rb"\[ASSERT\]",
    rb"\[ASSERT_ALWAYS\]",
    rb"Kernel panic",
    rb"panic:",
    rb"\[exception\] sync:",
    rb"=== BACKTRACE \(fault\)",
)


def run(ctx: TestContext, report: Report, arches) -> None:
    for arch in arches:
        _pmm(ctx, report, arch)
        _string(ctx, report, arch)
        _vmm(ctx, report, arch)


def _pmm(ctx, report, arch):
    def one(arch=arch):
        q = ctx.qemu(SUITE, arch, "pmm", variant="pmm-test", rootfs=False)
        try:
            q.wait_for(
                rb"=== All PMM Tests Passed ===",
                ctx.timeout(180),
                forbidden=FORBIDDEN,
                desc=f"{arch.name} PMM 自测",
            )
            # pmm_test 的保留区用例会**故意**触发一次
            # `refusing to free reserved`（并断言 free_pages 不变），
            # 所以这里不能把该日志当失败。
        finally:
            q.stop()

    ctx.case(
        report,
        SUITE,
        arch,
        "pmm",
        one,
        log=str(ctx.log_path(SUITE, arch, "pmm")),
    )


def _string(ctx, report, arch):
    def one(arch=arch):
        q = ctx.qemu(SUITE, arch, "string", variant="string-test", rootfs=False)
        try:
            m = q.wait_for(
                [rb"=== STRING TEST: PASS", rb"=== STRING TEST: \d+ FAILED"],
                ctx.timeout(120),
                forbidden=FORBIDDEN,
                desc=f"{arch.name} string 自测",
            )
            if b"FAILED" in m.group(0):
                raise AssertionError(m.group(0).decode("utf-8", "replace"))
        finally:
            q.stop()

    ctx.case(
        report,
        SUITE,
        arch,
        "string",
        one,
        log=str(ctx.log_path(SUITE, arch, "string")),
    )


def _vmm(ctx, report, arch):
    def one(arch=arch):
        if arch.needs_kvm_for_vmm and not ctx.kvm:
            raise SkipTest("x86_64 VMM_TEST 需要 KVM（/dev/kvm 不可用）")
        q = ctx.qemu(
            SUITE,
            arch,
            "vmm",
            variant="vmm-test",
            rootfs=False,
            kvm=ctx.kvm if arch.name == "x86_64" else None,
        )
        try:
            for marker in arch.vmm_markers:
                # 标记里有 `[` `]`，是字面量不是正则；不 escape 的话
                # `[VMM] HVC_PRINT` 会被当成"V 或 M 后跟空格"，永远匹配不上。
                q.wait_for(
                    re.escape(marker),
                    ctx.timeout(180),
                    forbidden=FORBIDDEN,
                    desc=f"{arch.name} VMM 标记 {marker!r}",
                )
        finally:
            q.stop()

    ctx.case(
        report,
        SUITE,
        arch,
        "vmm",
        one,
        log=str(ctx.log_path(SUITE, arch, "vmm")),
    )
