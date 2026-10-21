"""
ltp.py — 解析 run_ltp.sh 的逐例结果，并和已知失败基线比对。

run_ltp.sh 每个测例输出一行：
    LTP-CASE: <name> PASS|FAIL|SKIP|BROK

基线文件格式（tests/ltp/expected_failures.txt）：
    # arch testcase status
    x86_64 ioctl02 BROK
    *      somecase TCONF     # 星号表示所有架构
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path

CASE_RE = re.compile(r"LTP-CASE:\s+(\S+)\s+(PASS|FAIL|SKIP|BROK)")
SUMMARY_RE = re.compile(r"LTP Results:\s*(\d+)\s*tests")
COUNT_RE = re.compile(r"(PASS|FAIL|SKIP|BROK):\s*(\d+)")

BAD = {"FAIL", "BROK"}


@dataclass
class LtpResult:
    cases: dict[str, str] = field(default_factory=dict)
    summary: dict[str, int] = field(default_factory=dict)
    total: int | None = None

    def unexpected(self, baseline) -> dict[str, str]:
        return {
            name: status
            for name, status in self.cases.items()
            if status in BAD and not baseline.is_known(name, status)
        }

    def known(self, baseline) -> dict[str, str]:
        return {
            name: status
            for name, status in self.cases.items()
            if status in BAD and baseline.is_known(name, status)
        }


class Baseline:
    def __init__(self, entries=None, arch: str = ""):
        # (arch or "*", name) -> allowed statuses
        self.entries: dict[tuple[str, str], set[str]] = entries or {}
        self.arch = arch

    @classmethod
    def load(cls, path: Path, arch: str) -> "Baseline":
        entries: dict[tuple[str, str], set[str]] = {}
        if not path.is_file():
            return cls(entries, arch)
        for raw in path.read_text().splitlines():
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) < 3:
                continue
            a, name, status = parts[0], parts[1], parts[2].upper()
            if a in ("*", arch):
                entries.setdefault((a, name), set()).add(status)
        return cls(entries, arch)

    def is_known(self, name: str, status: str) -> bool:
        return status in self.entries.get(("*", name), set()) or status in self.entries.get(
            (self.arch, name), set()
        )


def parse(text: str) -> LtpResult:
    # 去掉 ANSI（调用方通常已经去过，这里再兜一层）
    text = re.sub(r"\x1b\[[0-9;?]*[ -/]*[@-~]", "", text)
    res = LtpResult()
    for m in CASE_RE.finditer(text):
        res.cases[m.group(1)] = m.group(2)
    m = SUMMARY_RE.search(text)
    if m:
        res.total = int(m.group(1))
    # 只取汇总区最后几行，避免把测例自己的输出混进来
    tail = text[-2000:]
    for m in COUNT_RE.finditer(tail):
        res.summary[m.group(1)] = int(m.group(2))
    return res
