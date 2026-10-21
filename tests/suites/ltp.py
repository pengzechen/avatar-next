"""
ltp suite — 三架构 LTP（宿主 Avatar OS 上跑，不是 guest Linux 里跑）。

- ltp-smoke：`tests/ltp/smoke.list`，PR 门禁用；
- ltp-mm：`tests/ltp/mm.list`，内存重构专用；
- ltp-full：跑 /ltp 下的全部测例。

判定：
  - 必须解析到逐例 LTP-CASE 行；
  - 不允许**新增** FAIL/BROK（expected_failures.txt 里登记过的已知失败除外）；
  - 请求的子集必须每个都出现结果（防止 rootfs 里装的是旧/不全的测例）。
"""

from __future__ import annotations

import pathlib
import shutil

from framework import ltp, platforms
from framework.context import SkipTest, TestContext
from framework.report import Report

SUITE = "ltp"
TESTS_DIR = pathlib.Path(__file__).resolve().parents[1]


def _load_subset(name: str):
    if name == "full":
        return None
    path = TESTS_DIR / "ltp" / f"{name}.list"
    if not path.is_file():
        raise SkipTest(f"缺少子集文件 {path}")
    names = []
    for raw in path.read_text().splitlines():
        line = raw.split("#", 1)[0].strip()
        if line:
            names.append(line)
    return names


def _prepare_bins(arch) -> pathlib.Path:
    bins = TESTS_DIR / "ltp" / "bin" / arch.name
    if not bins.is_dir() or not any(bins.iterdir()):
        raise SkipTest(f"缺少 LTP 二进制 {bins}（先跑 make -C tests ltp-build）")
    # build.sh 会拷一份 run_ltp.sh 到 bin/<arch>/；源码更新后必须重新拷，
    # 否则 rootfs 里还是旧脚本（没有 LTP-CASE 机器可读行）。
    src = TESTS_DIR / "ltp" / "run_ltp.sh"
    dst = bins / "run_ltp.sh"
    shutil.copyfile(src, dst)
    dst.chmod(0o755)
    return bins


LIST_OK = b"__AVATAR_LIST_OK__"


def _checked(q, cmd: str, timeout: float) -> None:
    """发一条短命令，并等它**执行完**（哨兵只会在执行后出现，回显里没有）。"""
    since = q.mark()
    q.send_slow((cmd + "; echo __AVATAR_LIST_OK__\n").encode())
    q.wait_for(LIST_OK, timeout, since=since, desc=f"命令 {cmd!r}")


def _rootfs_stale(arch, bins: pathlib.Path, ctx: TestContext) -> bool:
    rootfs = platforms.rootfs_path(ctx.artifacts, arch, "normal", ctx.smp)
    if not rootfs.is_file():
        return True
    try:
        newest = max(p.stat().st_mtime for p in bins.iterdir())
    except ValueError:
        return True
    return rootfs.stat().st_mtime < newest


def run(ctx: TestContext, report: Report, arches, subset_name: str = "smoke") -> None:
    subset = _load_subset(subset_name)
    for arch in arches:

        def one(arch=arch):
            bins = _prepare_bins(arch)
            ctx.ensure_build(
                arch,
                "normal",
                force=ctx.rebuild or _rootfs_stale(arch, bins, ctx),
            )
            log_path = ctx.log_path(SUITE, arch, subset_name)
            q = ctx.qemu(
                SUITE,
                arch,
                subset_name,
                variant="normal",
                rootfs=True,
                kvm=ctx.kvm if arch.name == "x86_64" else None,
            )
            try:
                q.wait_shell(ctx.timeout(180), desc=f"{arch.name} 宿主 shell")
                if subset:
                    # 不要把子集拼成一条长命令：宿主的串口控制台会按终端宽度
                    # 折行，实测会把 `LTP_TESTS="..."` 拆成两行，变量根本没
                    # 生效（结果跑成全量）。改成写一个小文件，每条命令都
                    # 等一个执行后的哨兵再发下一条 —— 连续 send 也会丢字节。
                    # 只用 shell 内建：不依赖 cat/tr/chmod 这些 applet，也不
                    # 用 `VAR="$(...)" cmd` 这种前缀赋值 —— 实测这个 busybox
                    # 在"前缀赋值 + 命令替换"组合下会把 cat 输出漏到终端且
                    # 变量没传进去，结果跑成全量。逐条追加、再 export 最稳。
                    _checked(q, 'LTP_TESTS=""', ctx.timeout(15))
                    for name in subset:
                        _checked(
                            q,
                            f'LTP_TESTS="$LTP_TESTS {name}"',
                            ctx.timeout(15),
                        )
                    # **source** 而不是 exec：宿主当前的 execve 没有把导出的
                    # 环境变量传给子进程（实测 `export X=...; sh -c 'echo $X'`
                    # 打印空）。source 在同一个 shell 里执行，变量可见。
                    q.send_line(". /ltp/run_ltp.sh")
                else:
                    q.send_line("/ltp/run_ltp.sh")
                q.wait_for(
                    rb"LTP Results:", ctx.timeout(1800), desc=f"{arch.name} LTP 汇总"
                )
                q.drain(0.5)
            finally:
                q.stop()

            text = log_path.read_text(errors="replace")
            res = ltp.parse(text)
            assert res.cases, (
                "没有解析到逐例 LTP-CASE 行；rootfs 里多半还是旧 run_ltp.sh"
                f"\n日志: {log_path}"
            )

            baseline = ltp.Baseline.load(
                TESTS_DIR / "ltp" / "expected_failures.txt", arch.name
            )
            unexpected = res.unexpected(baseline)
            known = res.known(baseline)

            if subset:
                missing = [n for n in subset if n not in res.cases]
                assert not missing, (
                    f"子集里有 {len(missing)} 个测例没有结果"
                    f"（rootfs 未安装或 run_ltp.sh 未执行）：{missing}"
                    f"\n日志: {log_path}"
                )

            assert not unexpected, (
                "新增 LTP 失败（未登记在 expected_failures.txt）："
                f"{unexpected}\n汇总: {res.summary}\n日志: {log_path}"
            )

            print(
                f"       LTP {arch.name}/{subset_name}: "
                f"cases={len(res.cases)} summary={res.summary} known={known}"
            )

        ctx.case(
            report,
            SUITE,
            arch,
            subset_name,
            one,
            log=str(ctx.log_path(SUITE, arch, subset_name)),
        )
