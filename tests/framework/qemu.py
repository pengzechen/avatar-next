"""
qemu.py — 用 PTY 驱动 QEMU 串口，提供 wait/send/expect。

设计要点（与现有 tools/*.sh、run_qemu_ltp.py 保持一致）：

- **不靠固定 sleep 猜启动时间**。等待提示符一律用探针命令，早发的丢了
  下一轮会重发；这是 TCG 下慢机器上唯一稳的做法。
- **只 kill 自己拉起的进程组**，绝不 pkill qemu-system-*（同一台机器上
  可能有别的会话正在调试）。
- 原始字节写进日志；匹配时去掉 ANSI 转义和 CR，便于写判据。
"""

from __future__ import annotations

import errno
import os
import pathlib
import pty
import re
import select
import signal
import subprocess
import time

# QEMU 的串口输出里会混 ANSI 颜色；匹配前统一清掉。
ANSI_RE = re.compile(rb"\x1b\[[0-9;?]*[ -/]*[@-~]")
MAX_MATCH_BUFFER = 4 << 20  # 4 MiB；足够覆盖启动/guest/LTP 的关键行


class QemuError(RuntimeError):
    pass


class QemuTimeout(QemuError):
    pass


def strip_ansi(data: bytes) -> bytes:
    return ANSI_RE.sub(b"", data).replace(b"\r", b"")


class Qemu:
    def __init__(self, cmd: list[str], log_path: pathlib.Path):
        self.cmd = cmd
        self.log_path = log_path
        log_path.parent.mkdir(parents=True, exist_ok=True)
        self._raw = open(log_path, "wb", buffering=0)
        self.master, slave = pty.openpty()
        self.proc = subprocess.Popen(
            cmd,
            stdin=slave,
            stdout=slave,
            stderr=slave,
            close_fds=True,
            start_new_session=True,
        )
        os.close(slave)
        self.buf = bytearray()
        self.eof = False
        self._stopped = False

    # ── 读取 ────────────────────────────────────────────────────
    def _pump(self, timeout: float) -> bool:
        try:
            r, _, _ = select.select([self.master], [], [], timeout)
        except InterruptedError:
            return False
        if self.master not in r:
            if self.proc.poll() is not None:
                self.eof = True
            return False
        try:
            data = os.read(self.master, 65536)
        except OSError as e:
            if e.errno == errno.EIO:  # PTY 对端已关闭
                data = b""
            else:
                raise
        if not data:
            self.eof = True
            return False
        self._raw.write(data)
        self.buf += strip_ansi(data)
        if len(self.buf) > MAX_MATCH_BUFFER:
            del self.buf[: len(self.buf) - MAX_MATCH_BUFFER]
        return True

    def _compile(self, pattern):
        if isinstance(pattern, re.Pattern):
            return pattern
        if isinstance(pattern, str):
            pattern = pattern.encode()
        return re.compile(pattern)

    def wait_for(
        self,
        patterns,
        timeout: float,
        *,
        forbidden=(),
        desc: str = "",
        since: int = 0,
    ) -> re.Match:
        """等任一 pattern 命中；命中 forbidden 立即失败。

        since: 从缓冲区这个偏移之后开始匹配（用于"停止后等新的提示符"）。
        """
        if isinstance(patterns, (bytes, str, re.Pattern)):
            patterns = [patterns]
        pats = [self._compile(p) for p in patterns]
        forb = [self._compile(p) for p in forbidden]
        deadline = time.monotonic() + timeout
        while True:
            text = bytes(self.buf[since:])
            for p in forb:
                m = p.search(text)
                if m:
                    raise QemuError(
                        f"{desc or 'QEMU'} 命中禁止模式 {p.pattern!r}: "
                        f"{m.group(0)[:200]!r}\n{self.tail()}"
                    )
            for p in pats:
                m = p.search(text)
                if m:
                    return m
            if time.monotonic() >= deadline:
                raise QemuTimeout(
                    f"{desc or 'QEMU'} 等待超时（{timeout:.0f}s）: "
                    f"{[p.pattern for p in pats]}\n{self.tail()}"
                )
            self._pump(0.2)
            if self.eof and self.proc.poll() is not None:
                # 再收到一次数据也可能命中；这里只报进程退出。
                text = bytes(self.buf[since:])
                if any(p.search(text) for p in pats):
                    continue
                raise QemuError(
                    f"{desc or 'QEMU'} 进程退出（rc={self.proc.returncode}），"
                    f"未命中 {[p.pattern for p in pats]}\n{self.tail()}"
                )

    def drain(self, seconds: float, *, forbidden=(), desc: str = "") -> None:
        """再观察一段时间，只检查 forbidden（用于"启动已成功、再等等看有没有后知后觉的异常"）。"""
        forb = [self._compile(p) for p in forbidden]
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self._pump(0.2)
            text = bytes(self.buf)
            for p in forb:
                m = p.search(text)
                if m:
                    raise QemuError(
                        f"{desc or 'QEMU'} 命中禁止模式 {p.pattern!r}: "
                        f"{m.group(0)[:200]!r}\n{self.tail()}"
                    )
            if self.eof and self.proc.poll() is not None:
                return

    def mark(self) -> int:
        return len(self.buf)

    def tail(self, lines: int = 25) -> str:
        text = bytes(self.buf).decode("utf-8", "replace")
        return "\n".join(text.splitlines()[-lines:])

    # ── 写入 ────────────────────────────────────────────────────
    def send(self, data: bytes) -> None:
        if self._stopped:
            return
        os.write(self.master, data)

    def send_line(self, line: str) -> None:
        self.send(line.encode() + b"\n")

    def send_slow(self, data: bytes, chunk: int = 8, delay: float = 0.02) -> None:
        """分块慢速写入。

        宿主的 busybox 控制台没有输入流控；一次 write 一大段（例如
        `LTP_TESTS="a b c ..." /ltp/run_ltp.sh`）会被 UART 丢掉中间
        字节，shell 于是停在半行命令上永远不执行。分块 + 小延迟是
        现有 shell 脚本里"等提示符再喂"的 Python 等价物。
        """
        if self._stopped:
            return
        for i in range(0, len(data), chunk):
            os.write(self.master, data[i : i + chunk])
            self._pump(0.0)
            if i + chunk < len(data):
                time.sleep(delay)

    # ── 常用等待 ────────────────────────────────────────────────
    def wait_shell(
        self,
        timeout: float = 180,
        *,
        desc: str = "shell",
        probe_every: float = 3.0,
        probe: bytes = b"echo __AVATAR_READY_$((6*7))__\n",
        mark: bytes = b"__AVATAR_READY_42__",
    ) -> None:
        """反复发探针，直到看到 shell **执行**后的标记。

        标记必须不同于回显文本：`echo __AVATAR_READY_$((6*7))__` 的回显里
        是字面量，只有执行后才出现 `__AVATAR_READY_42__`。
        """
        deadline = time.monotonic() + timeout
        next_probe = 0.0
        while True:
            now = time.monotonic()
            if now >= next_probe:
                self.send(probe)
                next_probe = now + probe_every
            self._pump(0.2)
            if mark in self.buf:
                return
            if now >= deadline:
                raise QemuTimeout(
                    f"{desc}: {timeout:.0f}s 内没等到 shell 探针 {mark!r}\n"
                    f"{self.tail()}"
                )
            if self.eof and self.proc.poll() is not None:
                raise QemuError(
                    f"{desc}: QEMU 提前退出（rc={self.proc.returncode}）\n"
                    f"{self.tail()}"
                )

    def expect_command_output(
        self, command: str, mark_expr: str, expected: bytes, timeout: float = 30
    ) -> None:
        """发一条会打印独特标记的命令，并等执行结果。"""
        self.send_line(command)
        self.wait_for(expected, timeout, desc=f"命令 {command!r} 的输出")

    # ── 收尾 ────────────────────────────────────────────────────
    def stop(self) -> None:
        if self._stopped:
            return
        self._stopped = True
        try:
            if self.proc.poll() is None:
                os.killpg(os.getpgid(self.proc.pid), signal.SIGTERM)
                try:
                    self.proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(os.getpgid(self.proc.pid), signal.SIGKILL)
                    self.proc.wait(timeout=5)
        finally:
            try:
                os.close(self.master)
            except OSError:
                pass
            self._raw.close()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.stop()
        return False
