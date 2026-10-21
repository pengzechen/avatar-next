#!/usr/bin/env python3
"""
run.py — Avatar OS 统一测试入口。

用法：
    python3 tests/run.py --suite build,boot,kernel
    python3 tests/run.py --suite ltp-smoke --arch aarch64
    python3 tests/run.py --suite vmm-direct,vmm-helper --smp 2
    python3 tests/run.py --list

设计约定：
  - 构建产物按 **变体 + SMP** 隔离在 tests/.artifacts/build/<variant>/smp<N>/；
  - 运行阶段不再隐式重建（除非 --rebuild）；
  - 只 kill 自己拉起的 QEMU 进程组；
  - 失败日志保留在 tests/.artifacts/runs/<run-id>/logs/ 下。
"""

from __future__ import annotations

import argparse
import pathlib
import sys

TESTS_DIR = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(TESTS_DIR))

from framework import platforms  # noqa: E402
from framework.context import TestContext  # noqa: E402
from framework.report import Report, new_run_id  # noqa: E402
from suites import apps, boot, build, kernel, ltp, racetest, vmm_guest  # noqa: E402

SUITE_ALIASES = {
    "check": ("build", "boot", "kernel", "apps"),
    "check-fast": ("build", "boot", "kernel", "apps"),
    "check-mm": (
        "build",
        "boot",
        "kernel",
        "apps",
        "ltp-mm",
        "vmm-direct",
        "vmm-helper",
    ),
    "all": (
        "build",
        "boot",
        "kernel",
        "apps",
        "racetest",
        "ltp-smoke",
        "ltp-mm",
        "ltp-full",
        "vmm-direct",
        "vmm-helper",
        "vmm-multivm",
    ),
}

SUITE_NAMES = (
    "build",
    "boot",
    "kernel",
    "apps",
    "racetest",
    "ltp-smoke",
    "ltp-mm",
    "ltp-full",
    "vmm-direct",
    "vmm-helper",
    "vmm-guest",
    "vmm-multivm",
)


def parse_suites(value: str) -> list[str]:
    out: list[str] = []
    for raw in value.split(","):
        name = raw.strip()
        if not name:
            continue
        if name in SUITE_ALIASES:
            for s in SUITE_ALIASES[name]:
                if s not in out:
                    out.append(s)
        elif name in SUITE_NAMES:
            if name not in out:
                out.append(name)
        else:
            raise SystemExit(
                f"未知 suite: {name}\n可用: {', '.join(SUITE_NAMES)}"
            )
    return out


def parse_arches(values) -> list:
    if not values:
        values = list(platforms.ARCH_NAMES)
    out = []
    for name in values:
        if name not in platforms.ARCHS:
            raise SystemExit(
                f"未知架构: {name}（可用: {', '.join(platforms.ARCH_NAMES)}）"
            )
        out.append(platforms.ARCHS[name])
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description="Avatar OS 统一测试入口")
    ap.add_argument("--suite", default="check", help="逗号分隔的 suite 名")
    ap.add_argument("--arch", nargs="+", help="aarch64 riscv64 x86_64")
    ap.add_argument("--artifacts", type=pathlib.Path, default=platforms.DEFAULT_ARTIFACTS)
    ap.add_argument(
        "--log",
        default="info",
        help="内核 LOG= 级别（默认 info；boot/kernel/helper 依赖 INFO 标记）",
    )
    ap.add_argument(
        "--smp",
        type=int,
        nargs="+",
        default=[1],
        help="一个或多个 SMP 值，例如 --smp 1 4",
    )
    ap.add_argument("--jobs", type=int, default=None, help="构建并行度")
    ap.add_argument("--timeout-scale", type=float, default=1.0)
    ap.add_argument("--rebuild", action="store_true", help="强制重新构建")
    ap.add_argument("--no-build", action="store_true", help="只用已有产物")
    ap.add_argument("--variants", default=None, help="build suite 的变体列表")
    ap.add_argument("--list", action="store_true", help="列出 suite")
    args = ap.parse_args()

    if args.list:
        print("可用 suite:")
        for name in SUITE_NAMES:
            print(f"  {name}")
        print("别名:")
        for name, members in SUITE_ALIASES.items():
            print(f"  {name} = {', '.join(members)}")
        return 0

    suites = parse_suites(args.suite)
    arches = parse_arches(args.arch)
    variants = None
    if args.variants:
        variants = [v.strip() for v in args.variants.split(",") if v.strip()]

    run_id = new_run_id()
    report = Report(run_id)
    base_run_dir = args.artifacts / "runs" / run_id

    print(
        f"=== Avatar OS tests  run={run_id}  arch={[a.name for a in arches]}  "
        f"smp={args.smp}  suites={suites} ==="
    )
    if arches and any(a.name == "x86_64" for a in arches):
        kvm = platforms.have_kvm()
        print(f"    KVM: {'可用' if kvm else '不可用（x86 VMM/guest 会 SKIP）'}")

    for smp in args.smp:
        ctx = TestContext(
            args.artifacts,
            f"{run_id}-smp{smp}",
            log=args.log,
            smp=smp,
            jobs=args.jobs,
            timeout_scale=args.timeout_scale,
            rebuild=args.rebuild,
            no_build=args.no_build,
        )
        print(f"\n########## SMP={smp} ##########")
        for suite in suites:
            print(f"\n── suite: {suite} (smp={smp}) ──")
            if suite == "build":
                build.run(ctx, report, arches, variants)
            elif suite == "boot":
                boot.run(ctx, report, arches)
            elif suite == "kernel":
                kernel.run(ctx, report, arches)
            elif suite == "apps":
                apps.run(ctx, report, arches)
            elif suite == "racetest":
                racetest.run(ctx, report, arches)
            elif suite in ("ltp-smoke", "ltp-mm", "ltp-full"):
                ltp.run(ctx, report, arches, subset_name=suite.split("-", 1)[1])
            elif suite == "vmm-direct":
                vmm_guest.run(ctx, report, arches, modes=("direct",))
            elif suite == "vmm-helper":
                vmm_guest.run(ctx, report, arches, modes=("helper",))
            elif suite == "vmm-guest":
                vmm_guest.run(ctx, report, arches, modes=("direct", "helper"))
            elif suite == "vmm-multivm":
                vmm_guest.run_multivm(ctx, report, arches)
            else:
                raise AssertionError(suite)

    report.print_summary()
    base_run_dir.mkdir(parents=True, exist_ok=True)
    report.write_json(base_run_dir / "report.json")
    report.write_markdown(base_run_dir / "report.md")
    print(f"报告: {base_run_dir / 'report.json'}")
    return 0 if report.ok else 1


if __name__ == "__main__":
    sys.exit(main())
