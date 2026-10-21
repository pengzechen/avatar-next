"""
platforms.py — 三架构的构建 / QEMU 启动定义。

测试框架的唯一目标之一：**绝不把不同变体的产物混在同一个 build 目录里**。
根 Makefile 的 BUILD_ROOT 可以在命令行覆盖，所以这里给每个变体一个独立的
BUILD_ROOT：

    tests/.artifacts/build/<variant>/<platform>/kernel_*.bin
    tests/.artifacts/build/<variant>/<platform>/rootfs-*.img

这样 guest / 普通 / VMM_TEST / PMM_TEST 之间不会互相覆盖（根 Makefile 的
`make rootfs` 会连带重编内核，见 CLAUDE.md 的变体警告）。
"""

from __future__ import annotations

import os
import pathlib
import re
import shutil
import subprocess
import time
from dataclasses import dataclass, field

TESTS_DIR = pathlib.Path(__file__).resolve().parents[1]
REPO_ROOT = TESTS_DIR.parent
DEFAULT_ARTIFACTS = TESTS_DIR / ".artifacts"

ARCH_NAMES = ("aarch64", "riscv64", "x86_64")


class BuildError(RuntimeError):
    pass


@dataclass(frozen=True)
class Arch:
    name: str
    platform: str
    kernel_bin: str
    rootfs_bin: str
    rootfs_addr: int
    qemu_base: tuple[str, ...]
    # x86 的 VMX / guest 测试必须要 KVM；TCG 跑不了项目当前的 x86 guest 路径。
    needs_kvm_for_vmm: bool = False
    # 交互测试的提示符（guest Linux 的 busybox）
    guest_prompt: bytes = b"~ # "
    # VMM_TEST 三路线程各自的日志标记
    vmm_markers: tuple[bytes, ...] = ()


def _platform_rootfs_base(platform: str, fallback: int) -> int:
    conf = REPO_ROOT / "platforms" / platform / "platform.conf"
    try:
        text = conf.read_text()
    except OSError:
        return fallback
    m = re.search(
        r"rootfs\s*=\s*\{[^}]*?\bbase\s*=\s*(0x[0-9a-fA-F]+|\d+)", text, re.S
    )
    return int(m.group(1), 0) if m else fallback


def _arch(name: str) -> Arch:
    if name == "aarch64":
        platform = "qemu-virt-aarch64"
        return Arch(
            name=name,
            platform=platform,
            kernel_bin="kernel_aarch64.bin",
            rootfs_bin="rootfs-aarch64.img",
            rootfs_addr=_platform_rootfs_base(platform, 0x5FE00000),
            qemu_base=(
                "qemu-system-aarch64",
                "-cpu",
                "cortex-a76",
                "-M",
                "virt,virtualization=on,gic-version=3",
            ),
            vmm_markers=(b"[el2_loop]", b"[VMM] HVC_PRINT", b"[el0]"),
        )
    if name == "riscv64":
        platform = "qemu-virt-riscv64"
        return Arch(
            name=name,
            platform=platform,
            kernel_bin="kernel_riscv64.bin",
            rootfs_bin="rootfs-riscv64.img",
            rootfs_addr=_platform_rootfs_base(platform, 0x88000000),
            qemu_base=(
                "qemu-system-riscv64",
                "-M",
                "virt",
                "-bios",
                "default",
            ),
            vmm_markers=(b"[rv_host]", b"[HEXT] ECALL_PRINT", b"[rv-user]"),
        )
    if name == "x86_64":
        platform = "qemu-virt-x86_64"
        return Arch(
            name=name,
            platform=platform,
            kernel_bin="kernel_x86_64.bin",
            rootfs_bin="rootfs-x86_64.img",
            rootfs_addr=_platform_rootfs_base(platform, 0x04000000),
            qemu_base=("qemu-system-x86_64", "-machine", "q35"),
            needs_kvm_for_vmm=True,
            vmm_markers=(b"[x86_host]", b"[VMX] VMCALL_PRINT", b"[user_test]"),
        )
    raise ValueError(f"unknown arch: {name}")


ARCHS: dict[str, Arch] = {name: _arch(name) for name in ARCH_NAMES}


@dataclass(frozen=True)
class Variant:
    name: str
    make_flags: tuple[str, ...]
    # 有序：guest 变体必须先 rootfs 再 kernel（见 Makefile 的 test-guest-linux）
    targets: tuple[tuple[str, ...], ...]
    needs_rootfs: bool = False
    needs_kernel: bool = True


VARIANTS: dict[str, Variant] = {
    "normal": Variant(
        "normal", (), (("kernel", "rootfs"),), needs_rootfs=True
    ),
    "guest": Variant(
        "guest",
        ("GUEST_LINUX=1",),
        (("rootfs",), ("kernel",)),
        needs_rootfs=True,
    ),
    "vmm-test": Variant("vmm-test", ("VMM_TEST=1",), (("kernel",),)),
    "string-test": Variant("string-test", ("STRING_TEST=1",), (("kernel",),)),
    "pmm-test": Variant("pmm-test", ("PMM_TEST=1",), (("kernel",),)),
    "panic-test": Variant("panic-test", ("PANIC_TEST=1",), (("kernel",),)),
}


def have_kvm() -> bool:
    if os.environ.get("AVATAR_TEST_NO_KVM"):
        return False
    return pathlib.Path("/dev/kvm").exists() and os.access("/dev/kvm", os.R_OK | os.W_OK)


def build_root(artifacts: pathlib.Path, variant: str, smp: int = 1) -> pathlib.Path:
    # SMP 也是编译期标志（CONFIG_SMP_CPUS），必须和变体一样隔离。
    return artifacts / "build" / variant / f"smp{smp}"


def build_dir(
    artifacts: pathlib.Path, arch: Arch, variant: str, smp: int = 1
) -> pathlib.Path:
    return build_root(artifacts, variant, smp) / arch.platform


def kernel_path(
    artifacts: pathlib.Path, arch: Arch, variant: str, smp: int = 1
) -> pathlib.Path:
    return build_dir(artifacts, arch, variant, smp) / arch.kernel_bin


def rootfs_path(
    artifacts: pathlib.Path, arch: Arch, variant: str, smp: int = 1
) -> pathlib.Path:
    return build_dir(artifacts, arch, variant, smp) / arch.rootfs_bin


def artifacts_ready(
    artifacts: pathlib.Path, arch: Arch, variant: Variant, smp: int = 1
) -> bool:
    if variant.needs_kernel and not kernel_path(
        artifacts, arch, variant.name, smp
    ).is_file():
        return False
    if variant.needs_rootfs and not rootfs_path(
        artifacts, arch, variant.name, smp
    ).is_file():
        return False
    return True


def _run_make(
    arch: Arch,
    variant: Variant,
    artifacts: pathlib.Path,
    log: str,
    smp: int,
    jobs: int,
    targets: tuple[str, ...],
    extra_flags: tuple[str, ...] = (),
) -> None:
    root = build_root(artifacts, variant.name, smp)
    root.mkdir(parents=True, exist_ok=True)
    cmd = [
        "make",
        f"PLATFORM={arch.platform}",
        f"BUILD_ROOT={root}",
        f"LOG={log}",
        f"SMP={smp}",
        f"-j{jobs}",
        *variant.make_flags,
        *extra_flags,
        *targets,
    ]
    log_dir = artifacts / "build-logs"
    log_dir.mkdir(parents=True, exist_ok=True)
    log_path = log_dir / f"{arch.name}-{variant.name}.log"

    t0 = time.monotonic()
    with open(log_path, "wb") as f:
        # 构建输出可能很长；成功时只留日志，失败时把尾部打到终端。
        proc = subprocess.run(
            cmd,
            cwd=REPO_ROOT,
            stdout=f,
            stderr=subprocess.STDOUT,
        )
    dt = time.monotonic() - t0
    if proc.returncode != 0:
        tail = "\n".join(log_path.read_text(errors="replace").splitlines()[-40:])
        raise BuildError(
            f"构建失败（{arch.name}/{variant.name}，{dt:.1f}s）\n"
            f"命令: {' '.join(cmd)}\n日志: {log_path}\n--- tail ---\n{tail}"
        )


def ensure_build(
    arch: Arch,
    variant_name: str,
    artifacts: pathlib.Path,
    *,
    log: str = "warn",
    smp: int = 1,
    jobs: int | None = None,
    force: bool = False,
) -> None:
    """按变体构建内核（以及 rootfs），产物落在该变体独立的 BUILD_ROOT。"""
    variant = VARIANTS[variant_name]
    if not force and artifacts_ready(artifacts, arch, variant, smp):
        return
    jobs = jobs or max(1, os.cpu_count() or 1)
    for targets in variant.targets:
        _run_make(arch, variant, artifacts, log, smp, jobs, targets)


def qemu_cmd(
    arch: Arch,
    kernel: pathlib.Path,
    *,
    rootfs: pathlib.Path | None = None,
    smp: int = 1,
    kvm: bool | None = None,
    extra: tuple[str, ...] = (),
) -> list[str]:
    """构造 QEMU 命令行。

    统一用 `-display none -serial stdio -monitor none`，避免 `-nographic`
    把 QEMU monitor 混到串口上（自动驱动不需要 monitor）。
    """
    cmd = list(arch.qemu_base)
    if arch.name == "x86_64":
        if kvm is None:
            kvm = have_kvm()
        if kvm:
            cmd += ["-enable-kvm", "-cpu", "host"]
        else:
            cmd += ["-cpu", "qemu64"]
    cmd += [
        "-smp",
        str(smp),
        "-m",
        "2G",
        "-display",
        "none",
        "-serial",
        "stdio",
        "-monitor",
        "none",
    ]
    cmd += ["-kernel", str(kernel)]
    if rootfs is not None:
        cmd += [
            "-device",
            f"loader,file={rootfs},addr=0x{arch.rootfs_addr:x},force-raw=on",
        ]
    cmd += list(extra)
    return cmd


def require_tool(name: str) -> bool:
    return shutil.which(name) is not None
