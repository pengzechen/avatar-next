"""
report.py — 测试结果收集与报告（控制台 / JSON / Markdown）。
"""

from __future__ import annotations

import json
import pathlib
import time
from dataclasses import dataclass, field

PASS = "PASS"
FAIL = "FAIL"
SKIP = "SKIP"
TIMEOUT = "TIMEOUT"
ENV_ERROR = "ENV-ERROR"

BAD_STATUSES = {FAIL, TIMEOUT, ENV_ERROR}


@dataclass
class CaseResult:
    suite: str
    arch: str
    name: str
    status: str
    duration: float
    log: str = ""
    detail: str = ""
    smp: int = 0
    meta: dict = field(default_factory=dict)


@dataclass
class Report:
    run_id: str
    results: list[CaseResult] = field(default_factory=list)

    def add(self, result: CaseResult) -> None:
        self.results.append(result)

    def counts(self) -> dict[str, int]:
        out: dict[str, int] = {}
        for r in self.results:
            out[r.status] = out.get(r.status, 0) + 1
        return out

    @property
    def ok(self) -> bool:
        return not any(r.status in BAD_STATUSES for r in self.results)

    def print_summary(self) -> None:
        if not self.results:
            print("没有执行任何测试。")
            return
        width = max(len(r.name) for r in self.results) + 2
        print()
        print("=" * 78)
        for r in self.results:
            line = (
                f"{r.status:<9} {r.arch:<8} smp{r.smp:<2} {r.suite:<12} "
                f"{r.name:<{width}} {r.duration:6.1f}s"
            )
            print(line)
            if r.detail and r.status in BAD_STATUSES:
                for ln in r.detail.strip().splitlines()[-8:]:
                    print(f"          {ln}")
            if r.log and r.status in BAD_STATUSES:
                print(f"          log: {r.log}")
        counts = self.counts()
        print("-" * 78)
        print(
            " ".join(f"{k}={v}" for k, v in sorted(counts.items())),
            "=>",
            "PASS" if self.ok else "FAIL",
        )
        print("=" * 78)

    def write_json(self, path: pathlib.Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        data = {
            "run_id": self.run_id,
            "ok": self.ok,
            "counts": self.counts(),
            "results": [r.__dict__ for r in self.results],
        }
        path.write_text(json.dumps(data, indent=2, ensure_ascii=False))

    def write_markdown(self, path: pathlib.Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        lines = [
            f"# Avatar OS test report `{self.run_id}`",
            "",
            f"结果：**{'PASS' if self.ok else 'FAIL'}**",
            "",
            "| 状态 | 架构 | SMP | Suite | 用例 | 耗时 |",
            "|---|---|---|---|---|---|",
        ]
        for r in self.results:
            lines.append(
                f"| {r.status} | {r.arch} | {r.smp} | {r.suite} | "
                f"{r.name} | {r.duration:.1f}s |"
            )
        lines.append("")
        path.write_text("\n".join(lines) + "\n")


def new_run_id() -> str:
    return time.strftime("%Y%m%d-%H%M%S")
