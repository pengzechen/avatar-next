"""
context.py — 测试运行的共享上下文：产物目录、构建缓存、QEMU 启动、结果包装。
"""

from __future__ import annotations

import pathlib
import time
import traceback

from . import platforms
from .platforms import Arch, BuildError
from .qemu import Qemu, QemuError, QemuTimeout
from .report import (
    ENV_ERROR,
    FAIL,
    PASS,
    SKIP,
    TIMEOUT,
    CaseResult,
    Report,
)


class SkipTest(Exception):
    pass


class TestContext:
    def __init__(
        self,
        artifacts: pathlib.Path,
        run_id: str,
        *,
        log: str = "info",
        smp: int = 1,
        jobs: int | None = None,
        timeout_scale: float = 1.0,
        rebuild: bool = False,
        no_build: bool = False,
    ):
        self.artifacts = artifacts
        self.run_id = run_id
        self.run_dir = artifacts / "runs" / run_id
        self.run_dir.mkdir(parents=True, exist_ok=True)
        self.log_level = log
        self.smp = smp
        self.jobs = jobs
        self.timeout_scale = timeout_scale
        self.rebuild = rebuild
        self.no_build = no_build
        self._built: set[tuple[str, str, int]] = set()
        self.kvm = platforms.have_kvm()

    # ── 构建 ────────────────────────────────────────────────────
    def ensure_build(self, arch: Arch, variant: str, *, force: bool = False) -> None:
        key = (arch.name, variant, self.smp)
        if not force and key in self._built:
            return
        if self.no_build:
            if not platforms.artifacts_ready(
                self.artifacts, arch, platforms.VARIANTS[variant], self.smp
            ):
                raise SkipTest(
                    f"--no-build：{arch.name}/{variant}/smp{self.smp} 产物不存在，"
                    "先跑 build suite"
                )
        else:
            platforms.ensure_build(
                arch,
                variant,
                self.artifacts,
                log=self.log_level,
                smp=self.smp,
                jobs=self.jobs,
                force=force or self.rebuild,
            )
        self._built.add(key)

    # ── QEMU ────────────────────────────────────────────────────
    def log_path(self, suite: str, arch: Arch, name: str) -> pathlib.Path:
        return self.run_dir / "logs" / suite / f"{arch.name}-{name}.log"

    def qemu(
        self,
        suite: str,
        arch: Arch,
        name: str,
        *,
        variant: str,
        rootfs: bool = False,
        rootfs_file: pathlib.Path | None = None,
        smp: int | None = None,
        kvm: bool | None = None,
        extra: tuple[str, ...] = (),
    ) -> Qemu:
        self.ensure_build(arch, variant)
        kernel = platforms.kernel_path(self.artifacts, arch, variant, self.smp)
        if not kernel.is_file():
            raise SkipTest(f"缺少内核产物 {kernel}")
        root = None
        if rootfs_file is not None:
            root = pathlib.Path(rootfs_file)
            if not root.is_file():
                raise SkipTest(f"缺少 rootfs 镜像 {root}")
        elif rootfs:
            root = platforms.rootfs_path(self.artifacts, arch, variant, self.smp)
            if not root.is_file():
                raise SkipTest(f"缺少 rootfs 产物 {root}")
        cmd = platforms.qemu_cmd(
            arch,
            kernel,
            rootfs=root,
            smp=smp if smp is not None else self.smp,
            kvm=kvm,
            extra=extra,
        )
        return Qemu(cmd, self.log_path(suite, arch, name))

    def timeout(self, base: float) -> float:
        return base * self.timeout_scale

    # ── 结果包装 ────────────────────────────────────────────────
    def case(
        self,
        report: Report,
        suite: str,
        arch: Arch,
        name: str,
        fn,
        *,
        log: str = "",
    ) -> CaseResult:
        t0 = time.monotonic()
        status = PASS
        detail = ""
        try:
            fn()
        except SkipTest as e:
            status, detail = SKIP, str(e)
        except QemuTimeout as e:
            status, detail = TIMEOUT, str(e)
        except BuildError as e:
            status, detail = FAIL, str(e)
        except QemuError as e:
            status, detail = FAIL, str(e)
        except (FileNotFoundError, PermissionError, OSError) as e:
            status, detail = ENV_ERROR, f"{type(e).__name__}: {e}"
        except AssertionError as e:
            status, detail = FAIL, str(e) or "assertion failed"
        except Exception:
            status, detail = FAIL, traceback.format_exc()
        dt = time.monotonic() - t0
        result = CaseResult(
            suite=suite,
            arch=arch.name,
            name=name,
            status=status,
            duration=dt,
            log=log,
            detail=detail,
            smp=self.smp,
        )
        report.add(result)
        print(f"  [{status:<9}] {arch.name:<8} {suite}/{name} ({dt:.1f}s)")
        return result
