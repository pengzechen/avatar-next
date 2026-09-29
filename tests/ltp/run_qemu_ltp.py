#!/usr/bin/env python3
"""run_qemu_ltp.py — 无人值守跑 LTP：驱动 QEMU stdio，自动敲 /ltp/run_ltp.sh

用法:
    python3 tests/ltp/run_qemu_ltp.py <qemu 命令行...>

    建议由 make 提供命令行：
        QEMU_CMD=$(make PLATFORM=qemu-virt-x86_64 -n test-ltp | tail -1)
        python3 tests/ltp/run_qemu_ltp.py $QEMU_CMD

为什么要这个脚本：run_ltp.sh 是交互式 shell 里手敲的，回归时没法自动化。
QEMU 用 -nographic（串口接 stdio），所以可以用 pty 驱动：等 shell 起来 →
敲命令 → 收输出到汇总行 → 退出。

不做固定 sleep 猜启动时间：那样要么太早（UART 还没初始化，字节丢掉），
要么太慢（白等）。改成**反复发一个探针** `echo __AVATAR_READY__`，直到在
输出里看见回声为止 —— 早发的丢了也没关系，下一轮会重发。

输出原样透传到 stdout，方便直接重定向到日志。
"""

import os
import pty
import select
import subprocess
import sys
import time

# 探针必须做到「回显的文本」和「执行的输出」**不同**，否则分不清是 shell 执行了
# 还是 pty 只是把输入回显了。这里让 shell 自己算一下：
#   回显：  echo __AVATAR_READY_$((6*7))__
#   输出：  __AVATAR_READY_42__
# 只认后者。踩过两次坑：
#   1) 用 `echo __AVATAR_READY__` + 子串匹配 → pty 回显就把标记带出来了，
#      QEMU 启动失败（rootfs 镜像不存在）时也判成"已就绪"。
#   2) 改成"要求标记在行首(\n 前缀)" → 仍然失败，因为内核 klog 的 ANSI 复位码
#      让实际字节是 `\x1b[0m__AVATAR_READY__`，marker 前面根本不是换行。
PROBE = b"echo __AVATAR_READY_$((6*7))__\n"
PROBE_MARK = b"__AVATAR_READY_42__"
RUN_CMD = os.environ.get("RUN_CMD", "/ltp/run_ltp.sh").encode() + b"\n"
DONE_MARK = os.environ.get("DONE_MARK", "LTP Results:").encode()

# 探针重发间隔 / 总超时 / 发现完成后再 drain 多久
PROBE_EVERY = 3.0
BOOT_TIMEOUT = float(os.environ.get("BOOT_TIMEOUT", "180"))
RUN_TIMEOUT = float(os.environ.get("RUN_TIMEOUT", "1800"))
DRAIN_AFTER_DONE = float(os.environ.get("DRAIN_AFTER_DONE", "8"))


def die(msg):
    print("\n[driver] %s" % msg, file=sys.stderr)
    sys.exit(2)


def main():
    cmd = sys.argv[1:]
    if not cmd:
        die("没有给 QEMU 命令行")

    master, slave = pty.openpty()
    proc = subprocess.Popen(
        cmd, stdin=slave, stdout=slave, stderr=slave, close_fds=True
    )
    os.close(slave)

    out = sys.stdout.buffer
    seen = bytearray()
    t0 = time.time()
    last_probe = 0.0
    sent_run = False
    done_at = None

    try:
        while True:
            now = time.time()

            if done_at is not None and now - done_at > DRAIN_AFTER_DONE:
                print("\n[driver] LTP 汇总已出现，收工", file=sys.stderr)
                break
            if not sent_run and now - t0 > BOOT_TIMEOUT:
                die("等了 %ds 没等到 shell 探针回声" % BOOT_TIMEOUT)
            if sent_run and done_at is None and now - t0 > (
                BOOT_TIMEOUT + RUN_TIMEOUT
            ):
                die("LTP 跑了 %ds 还没出汇总行" % RUN_TIMEOUT)

            r, _, _ = select.select([master], [], [], 0.5)
            if master in r:
                try:
                    data = os.read(master, 65536)
                except OSError:
                    break
                if not data:
                    print(
                        "\n[driver] QEMU 的 pty 到 EOF 了（进程已退出）",
                        file=sys.stderr,
                    )
                    break
                out.write(data)
                out.flush()
                seen += data
                # 只保留尾部，避免长时间运行下无界增长；前面补一个 '\n' ，
                # 免得截断后恰好把标记顶到 buffer 开头而漏判
                if len(seen) > 1 << 20:
                    del seen[: len(seen) - (1 << 19)]
                    seen[:0] = b"\n"

            if not sent_run:
                if PROBE_MARK in seen:
                    os.write(master, RUN_CMD)
                    sent_run = True
                    print("\n[driver] shell 已就绪，开始跑 LTP\n", file=sys.stderr)
                elif now - last_probe > PROBE_EVERY:
                    os.write(master, PROBE)
                    last_probe = now
            elif done_at is None and DONE_MARK in seen:
                done_at = time.time()

            if proc.poll() is not None:
                print(
                    "\n[driver] QEMU 自己退出了 (rc=%s)" % proc.returncode,
                    file=sys.stderr,
                )
                break
    finally:
        # 只杀自己拉起来的这个进程，不要 pkill qemu-system-*：
        # 同一台机器上别的会话可能正在用 QEMU 调试。
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        os.close(master)

    # 退出码要能区分"跑完了"和"根本没跑起来"。否则 QEMU 启动失败（比如 rootfs
    # 镜像不存在）也会以 0 退出，看起来像成功 —— 实测被骗过一次。
    if done_at is None:
        print(
            "[driver] 没有跑到 LTP 汇总行，判定为未完成（不是全绿）",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())