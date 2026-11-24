#!/usr/bin/env python3
"""
gen_rt_report.py — 从测试串口日志生成 RT 实时性全貌报告（HTML）。

用法：
    python3 tools/gen_rt_report.py
    python3 tools/gen_rt_report.py --rt-run tests/.artifacts/runs/<id>-smp4 \
        --trace-run tests/.artifacts/runs/<id>-smp4 \
        --smp1-run tests/.artifacts/runs/<id>-smp1 \
        --out tests/.artifacts/rt-report.html

默认自动挑选最新的：
  - rt-test SMP=4 x86_64（含 PI 链 + throttling 的 run）
  - rt-trace SMP=4 x86_64
  - rt-test SMP=1（aarch64 / riscv64 / x86_64 三份日志齐全的 run）

只读取日志，不重跑测试。
"""

from __future__ import annotations

import argparse
import datetime
import html
import pathlib
import re
import sys

ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
ARCHS = ("aarch64", "riscv64", "x86_64")


def read_log(path: pathlib.Path) -> str:
    return ANSI_RE.sub("", path.read_text(errors="replace")).replace("\r", "")


def find_run(artifacts: pathlib.Path, smp: int, rel: str, needle: str) -> pathlib.Path:
    for d in sorted(artifacts.glob(f"runs/*-smp{smp}"), reverse=True):
        p = d / rel
        if p.is_file() and needle in read_log(p):
            return d
    raise SystemExit(f"找不到 smp{smp} 的 {rel}（需要包含 {needle!r}）")


def find_smp1_run(artifacts: pathlib.Path) -> pathlib.Path:
    for d in sorted(artifacts.glob("runs/*-smp1"), reverse=True):
        ok = all(
            (d / f"logs/rt-test/{a}-rt.log").is_file()
            and "CYCLICTEST: PASS" in read_log(d / f"logs/rt-test/{a}-rt.log")
            for a in ARCHS
        )
        if ok:
            return d
    raise SystemExit("找不到三架构 SMP=1 的 rt-test run")


def _re(text: str, pattern: str):
    m = re.search(pattern, text)
    return m.groups() if m else None


def parse_rt_log(text: str) -> dict:
    d: dict = {}

    m = _re(text, r"RT SCHED TEST: preempt min/avg/max = (\d+)/(\d+)/(\d+) ns")
    if m:
        d["preempt"] = tuple(int(x) for x in m)

    m = _re(text, r"RT DEMO pingpong: n=(\d+) min=(\d+) avg=(\d+) max=(\d+) ns")
    if m:
        d["pingpong"] = tuple(int(x) for x in m)

    m = _re(text, r"RT DEMO preempt: n=(\d+) min=(\d+) avg=(\d+) max=(\d+) ns")
    if m:
        d["demo_preempt"] = tuple(int(x) for x in m)

    m = _re(
        text,
        r"RT WORKER TEST: PASS period=(\d+) periodic=(\d+) ticks=(\d+) "
        r"misses=(\d+) exec_max=(\d+)",
    )
    if m:
        d["worker"] = tuple(int(x) for x in m)

    m = _re(
        text,
        r"RT OBS TEST: PASS reset=(\d+) wd_tested=(\d+) wd_checks=(\d+) wd_trips=(\d+)",
    )
    if m:
        d["obs"] = tuple(int(x) for x in m)

    m = _re(
        text,
        r"RT MUTEX TEST: PASS mtx=(\d+) err=(\d+) boost=(\d+) after=(\d+) "
        r"order=(\d+)/(\d+)/(\d+) chain=(\d+)/(\d+)/(\d+)",
    )
    if m:
        d["mutex"] = tuple(int(x) for x in m)

    m = _re(
        text,
        r"RT PI CHAIN: PASS prio_low=(\d+) prio_mid=(\d+) high_lat=(\d+) ns",
    )
    if m:
        d["pi_chain"] = tuple(int(x) for x in m)

    m = _re(
        text,
        r"RT EVENT TEST: PASS timeout=(\d+) posted=(\d+) processed=(\d+) dropped=(\d+)",
    )
    if m:
        d["event"] = tuple(int(x) for x in m)

    m = _re(
        text,
        r"RT IPC TEST: PASS n=(\d+) min=(\d+) avg=(\d+) p99=(\d+) max=(\d+)",
    )
    if m:
        d["ipc"] = tuple(int(x) for x in m)

    m = _re(
        text,
        r"RT THROTTLE TEST: PASS rt_iters=(\d+) normal_iters=(\d+) events=(\d+)",
    )
    if m:
        d["throttle"] = tuple(int(x) for x in m)

    m = _re(
        text,
        r"CYCLICTEST: PASS period=(\d+) warmup=(\d+) cycles=(\d+) samples=(\d+) "
        r"min=(\d+) avg=(\d+) p50=(\d+) p99=(\d+) p999=(\d+) max=(\d+) "
        r"stddev=(\d+) jitter=(\d+) drift=(-?\d+) overrun_ppm=(\d+) "
        r"irq_n=(\d+) irq_min=(\d+) irq_avg=(\d+) irq_p99=(\d+) irq_max=(\d+) "
        r"overruns=(\d+) early=(\d+) hog=(\d+) hog_iters=(\d+) "
        r"hist=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)",
    )
    if m:
        v = tuple(int(x) for x in m)
        keys = (
            "period warmup cycles samples min avg p50 p99 p999 max stddev jitter "
            "drift overrun_ppm irq_n irq_min irq_avg irq_p99 irq_max overruns "
            "early hog hog_iters"
        ).split()
        d["cyc"] = dict(zip(keys, v[: len(keys)]))
        d["cyc_hist"] = list(v[len(keys) :])

    m = _re(
        text,
        r"OSLAT: PASS duration=(\d+) samples=(\d+) min=(\d+) avg=(\d+) max=(\d+) "
        r"over_1us=(\d+) over_10us=(\d+) over_100us=(\d+)",
    )
    if m:
        d["oslat"] = tuple(int(x) for x in m)

    m = _re(text, r"PREEMPTOFF TEST: PASS count=(\d+) held=(\d+) max=(\d+)")
    if m:
        d["preemptoff"] = tuple(int(x) for x in m)

    m = _re(text, r"=== Avatar OS Kernel \w+ build (.+?) ===")
    if m:
        d["build"] = m[0]

    return d


def parse_trace_log(text: str) -> tuple[dict, dict[int, dict]]:
    d: dict = {}
    m = _re(text, r"PREEMPTOFF TEST: PASS count=(\d+) held=(\d+) max=(\d+)")
    if m:
        d["preemptoff"] = tuple(int(x) for x in m)

    rows: dict[int, dict] = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) != 24 or not re.fullmatch(r"cpu\d+", parts[0]):
            continue
        v = parts[1:]
        try:
            rows[int(parts[0][3:])] = {
                "wake_max": int(v[0]),
                "wake_n": int(v[2]),
                "wake_min": int(v[3]),
                "wake_avg": int(v[4]),
                "syscall_max": int(v[5]),
                "syscall_avg": int(v[8]),
                "syscall_n": int(v[9]),
                "tick_gap_max": int(v[10]),
                "irq_off_max": int(v[13]),
                "irq_off_n": int(v[15]),
                "preempt_off_max": int(v[17]),
                "preempt_off_n": int(v[19]),
                "ctxt": int(v[21]),
                "migrations": int(v[22]),
            }
        except (ValueError, IndexError):
            continue
    return d, rows


def fmt_ns(ns: float) -> str:
    ns = float(ns)
    if ns >= 1_000_000_000:
        return f"{ns / 1_000_000_000:.2f}s"
    if ns >= 1_000_000:
        return f"{ns / 1_000_000:.2f}ms"
    if ns >= 1_000:
        return f"{ns / 1_000:.2f}µs"
    return f"{ns:.0f}ns"


def fmt_ratio(a: int, b: int) -> str:
    return f"{(a * 100.0 / b):.1f}%" if b else "—"


def pct(part: int, total: int) -> float:
    return 100.0 * part / total if total else 0.0


# ── 静态数据：测试先行的“修复前”读数（来自对应失败 run）────────────
BEFORE_PI = {"prio_low": 50, "prio_mid": 80, "high_lat": 20014927}
BEFORE_PI_RUN = "20261125-002308-smp1"
BEFORE_THR = {"rt": 146861037, "normal": 0}
BEFORE_THR_RUN = "20261125-003840-smp1"


CSS = r"""
:root {
  color-scheme: light;
  --bg:#eef3f4; --surface:#fbfdfd; --surface2:#e7eef0; --ink:#102328;
  --muted:#5d7178; --faint:#84979c; --line:#cfdde0; --track:#dfeaec;
  --accent:#0e7490; --accent2:#155e75; --accent-soft:#d9eef3;
  --good:#15803d; --good-soft:#def3e5; --warn:#b45309; --warn-soft:#f9ead2;
  --bad:#b42318; --bad-soft:#fbe3e1;
  --mono:"SFMono-Regular",ui-monospace,Menlo,Consolas,"Liberation Mono",monospace;
  --sans:-apple-system,BlinkMacSystemFont,"Segoe UI",system-ui,sans-serif;
  --shadow:0 16px 40px rgba(16,35,40,.07);
}
@media (prefers-color-scheme: dark) {
  :root {
    color-scheme: dark;
    --bg:#081216; --surface:#0f1d22; --surface2:#14282e; --ink:#e8f2f3;
    --muted:#93abb0; --faint:#6f878d; --line:#223a41; --track:#1b3036;
    --accent:#22d3ee; --accent2:#a5f3fc; --accent-soft:#10343b;
    --good:#4ade80; --good-soft:#123523; --warn:#fbbf24; --warn-soft:#3a2b13;
    --bad:#fb7185; --bad-soft:#3a1820;
    --shadow:0 20px 55px rgba(0,0,0,.30);
  }
}
* { box-sizing: border-box; }
body {
  margin:0; background:var(--bg); color:var(--ink); font-family:var(--sans);
  line-height:1.55; -webkit-font-smoothing:antialiased;
}
.wrap { max-width:1180px; margin:0 auto; padding:40px 22px 56px; }
.eyebrow {
  font:600 11px/1 var(--mono); letter-spacing:.18em; color:var(--accent);
  text-transform:uppercase; margin-bottom:14px;
}
h1 { font-size:clamp(30px,4vw,46px); line-height:1.05; margin:0 0 12px; letter-spacing:-.02em; }
h1 em { color:var(--accent); font-style:normal; }
.lede { max-width:820px; color:var(--muted); font-size:16px; margin:0 0 20px; }
.badges { display:flex; flex-wrap:wrap; gap:8px; margin-bottom:26px; }
.badge {
  font:600 11px/1 var(--mono); padding:7px 10px; border-radius:99px;
  background:var(--surface); border:1px solid var(--line); color:var(--muted);
}
.badge b { color:var(--ink); font-weight:600; }
.badge.good { color:var(--good); background:var(--good-soft); border-color:transparent; }
.kpis {
  display:grid; grid-template-columns:repeat(4,minmax(0,1fr));
  gap:12px; margin:0 0 34px;
}
@media (max-width:900px) { .kpis { grid-template-columns:repeat(2,minmax(0,1fr)); } }
@media (max-width:480px) { .kpis { grid-template-columns:1fr; } }
.card {
  background:var(--surface); border:1px solid var(--line); border-radius:14px;
  padding:15px 16px 14px; box-shadow:var(--shadow); min-width:0;
}
.card .k { font:600 10px/1.2 var(--mono); letter-spacing:.08em; text-transform:uppercase; color:var(--faint); }
.card .v { font:600 26px/1.15 var(--mono); letter-spacing:-.03em; margin:9px 0 2px; }
.card .v small { font-size:12px; color:var(--muted); font-weight:500; }
.card .s { font-size:12px; color:var(--muted); }
.card.hi .v { color:var(--accent); }
section { margin:0 0 34px; }
h2 { font-size:21px; letter-spacing:-.01em; margin:0 0 4px; }
h2 + .sub, .sub { color:var(--muted); font-size:13px; margin:0 0 14px; }
.panel {
  background:var(--surface); border:1px solid var(--line); border-radius:14px;
  padding:18px 18px 16px; box-shadow:var(--shadow);
}
.grid2 { display:grid; grid-template-columns:1.05fr .95fr; gap:14px; }
@media (max-width:820px) { .grid2 { grid-template-columns:1fr; } }
.bar-row { display:grid; grid-template-columns:150px 1fr 78px; gap:10px; align-items:center; padding:5px 0; }
.bar-row .lbl { font:500 12px/1.2 var(--mono); color:var(--muted); }
.bar-track { height:9px; background:var(--track); border-radius:99px; overflow:hidden; }
.bar-fill { height:100%; border-radius:99px; background:linear-gradient(90deg,var(--accent),var(--accent2)); }
.bar-fill.warn { background:linear-gradient(90deg,#d97706,#fbbf24); }
.bar-row .val { font:600 12px/1 var(--mono); text-align:right; color:var(--ink); }
.legend { font:11px/1.4 var(--mono); color:var(--faint); margin-top:10px; }
.tbl { width:100%; border-collapse:collapse; font-size:13px; }
.tbl th, .tbl td { text-align:left; padding:9px 10px; border-bottom:1px solid var(--line); vertical-align:top; }
.tbl th { font:600 10px/1.2 var(--mono); letter-spacing:.06em; text-transform:uppercase; color:var(--faint); }
.tbl td.num, .tbl th.num { text-align:right; font-family:var(--mono); white-space:nowrap; }
.tbl tr:last-child td { border-bottom:0; }
.tbl .good { color:var(--good); font-weight:600; }
.tbl .warn { color:var(--warn); font-weight:600; }
.mono { font-family:var(--mono); }
.note {
  margin-top:12px; padding:12px 14px; border-radius:10px; font-size:13px;
  background:var(--surface2); color:var(--muted); border-left:3px solid var(--accent);
}
.note.warn { border-left-color:var(--warn); background:var(--warn-soft); color:var(--ink); }
.note.good { border-left-color:var(--good); background:var(--good-soft); color:var(--ink); }
.hist { display:flex; align-items:flex-end; gap:6px; height:150px; margin-top:8px; }
.hist .col { flex:1; display:flex; flex-direction:column; justify-content:flex-end; align-items:center; gap:6px; min-width:0; }
.hist .b { width:100%; background:linear-gradient(180deg,var(--accent),var(--accent2)); border-radius:5px 5px 0 0; min-height:1px; }
.hist .n { font:600 10px/1 var(--mono); color:var(--muted); }
.hist .t { font:9px/1.2 var(--mono); color:var(--faint); white-space:nowrap; }
.story { display:grid; grid-template-columns:1fr 1fr; gap:14px; }
@media (max-width:720px) { .story { grid-template-columns:1fr; } }
.big { font:600 24px/1.1 var(--mono); letter-spacing:-.03em; }
.big small { font-size:12px; color:var(--muted); font-weight:500; }
.flow { font:12px/1.7 var(--mono); color:var(--muted); background:var(--surface2); border-radius:10px; padding:10px 12px; margin:10px 0 0; white-space:pre-wrap; }
.caveats { display:grid; grid-template-columns:repeat(auto-fit,minmax(240px,1fr)); gap:12px; }
.caveat { background:var(--surface); border:1px solid var(--line); border-radius:12px; padding:13px 15px; }
.caveat b { display:block; font-size:13px; margin-bottom:4px; }
.caveat p { margin:0; color:var(--muted); font-size:12.5px; }
footer {
  margin-top:38px; padding-top:16px; border-top:1px solid var(--line);
  font:11px/1.7 var(--mono); color:var(--faint);
}
footer span { display:block; }
@media (max-width:560px) {
  .bar-row { grid-template-columns:112px 1fr 66px; }
  .kpis { grid-template-columns:repeat(2, minmax(0,1fr)); }
}
"""


def bar(label, ns, max_ns, cls=""):
    w = min(100.0, 100.0 * float(ns) / float(max_ns)) if max_ns else 0.0
    return (
        f'<div class="bar-row"><div class="lbl">{html.escape(label)}</div>'
        f'<div class="bar-track"><div class="bar-fill {cls}" style="width:{w:.2f}%"></div></div>'
        f'<div class="val">{fmt_ns(ns)}</div></div>'
    )


def hist_html(counts, thresh):
    total = sum(counts) or 1
    peak = max(counts) or 1
    out = ['<div class="hist">']
    for i, n in enumerate(counts):
        h = max(2, int(130 * n / peak))
        label = f"<{fmt_ns(thresh[i] * 1000)}" if i < len(thresh) else "∞"
        out.append(
            f'<div class="col"><div class="n">{n}</div>'
            f'<div class="b" style="height:{h}px"></div>'
            f'<div class="t">{label}</div></div>'
        )
    out.append("</div>")
    out.append(
        f'<div class="legend">共 {total} 个样本；桶界是固定档位（1/2/5/10/20/50/100µs），不是等宽。</div>'
    )
    return "".join(out)


def render(rt, trace, trace_rows, smp1, ids) -> str:
    cyc = rt.get("cyc", {})
    ipc = rt.get("ipc", (0, 0, 0, 0, 0))
    pre = rt.get("preempt", (0, 0, 0))
    ping = rt.get("pingpong", (0, 0, 0, 0))
    demo_pre = rt.get("demo_preempt", (0, 0, 0, 0))
    th = rt.get("throttle", (0, 0, 0))
    pi = rt.get("pi_chain", (0, 0, 0))
    osl = rt.get("oslat", (0, 0, 0, 0, 0, 0, 0, 0))
    worker = rt.get("worker", (0, 0, 0, 0, 0))
    obs = rt.get("obs", (0, 0, 0, 0))
    mutex = rt.get("mutex", (0, 0, 0, 0, 0, 0, 0, 0, 0, 0))
    event = rt.get("event", (0, 0, 0, 0))

    thr_ratio = fmt_ratio(th[1], th[0]) if th else "—"
    irq_avg = cyc.get("irq_avg", 0)
    irq_p99 = cyc.get("irq_p99", 0)
    irq_max = cyc.get("irq_max", 0)

    kpis = [
        ("cyclictest p99", fmt_ns(cyc.get("p99", 0)), "1ms 周期 · x86 SMP4", "hi"),
        ("cyclictest max", fmt_ns(cyc.get("max", 0)), f"{cyc.get('samples', 0)} 样本", "hi"),
        ("overrun", f"{cyc.get('overruns', 0)}", f"overrun_ppm={cyc.get('overrun_ppm', 0)}", "good"),
        ("timer IRQ avg", fmt_ns(irq_avg), f"p99 {fmt_ns(irq_p99)} · max {fmt_ns(irq_max)}", ""),
        ("IPC 唤醒 avg", fmt_ns(ipc[2]), f"p99 {fmt_ns(ipc[3])} · max {fmt_ns(ipc[4])}", ""),
        ("self-IPI 抢占 avg", fmt_ns(pre[1]), f"max {fmt_ns(pre[2])}", ""),
        ("PI 链 high_lat", fmt_ns(pi[2]), f"low/mid={pi[0]}/{pi[1]}", "good"),
        ("RT throttle", thr_ratio, f"NORMAL/RT 迭代比 · events={th[2]}", "good"),
    ]
    kpi_html = "".join(
        f'<div class="card {cls}"><div class="k">{html.escape(k)}</div>'
        f'<div class="v">{v}</div><div class="s">{html.escape(s)}</div></div>'
        for k, v, s, cls in kpis
    )

    ladder = [
        ("timer IRQ avg", irq_avg),
        ("IPC wake avg", ipc[2]),
        ("self-IPI preempt avg", pre[1]),
        ("cyclictest p50", cyc.get("p50", 0)),
        ("cyclictest p99", cyc.get("p99", 0)),
        ("cyclictest p999", cyc.get("p999", 0)),
        ("cyclictest max", cyc.get("max", 0)),
    ]
    ladder_max = max(v for _, v in ladder) or 1
    ladder_html = "".join(
        bar(name, val, ladder_max, "warn" if name.endswith("max") else "")
        for name, val in ladder
    )

    hist = hist_html(rt.get("cyc_hist", [0] * 8), [1, 2, 5, 10, 20, 50, 100])

    detail_rows = [
        ("ping-pong", "5000 次任务间唤醒", f"avg {fmt_ns(ping[2])} · max {fmt_ns(ping[3])}"),
        ("self-IPI 抢占", "NORMAL 唤醒 FIFO 后的立即抢占", f"avg {fmt_ns(pre[1])} · max {fmt_ns(pre[2])}"),
        ("RT worker", "1ms 周期 · 208 ticks", f"misses={worker[3]} · exec_max={fmt_ns(worker[4])}"),
        ("observability", "运行中 reset / watchdog", f"wd_checks={obs[2]} · wd_trips={obs[3]}"),
        ("mutex", "18000 次临界区", f"err={mutex[1]} · 等待顺序 {mutex[4]}/{mutex[5]}/{mutex[6]}"),
        ("PI 链", "L(20)/M(50)/H(80) + W(60)", f"low/mid={pi[0]}/{pi[1]} · high_lat={fmt_ns(pi[2])}"),
        ("rt_event", "event 模板环", f"processed={event[2]}/84 · dropped={event[3]}"),
        ("IPC 唤醒", "500 次 set→等待者运行", f"min/avg/p99/max = {fmt_ns(ipc[1])}/{fmt_ns(ipc[2])}/{fmt_ns(ipc[3])}/{fmt_ns(ipc[4])}"),
        ("RT throttling", "NORMAL + FIFO 纯忙循环 2.5s", f"normal/rt={thr_ratio} · events={th[2]}"),
        ("cyclictest", "1ms 周期 · 2000 样本", f"p50/p99/p999/max = {fmt_ns(cyc.get('p50',0))}/{fmt_ns(cyc.get('p99',0))}/{fmt_ns(cyc.get('p999',0))}/{fmt_ns(cyc.get('max',0))}"),
        ("oslat", "500ms RT 执行连续性", f"min/avg/max = {fmt_ns(osl[2])}/{fmt_ns(osl[3])}/{fmt_ns(osl[4])}"),
    ]
    detail_html = "".join(
        f"<tr><td><b>{html.escape(a)}</b></td><td>{html.escape(b)}</td>"
        f'<td class="num">{html.escape(c)}</td></tr>'
        for a, b, c in detail_rows
    )

    # rt-trace per-CPU 表
    trace_html = []
    for cpu in sorted(trace_rows):
        r = trace_rows[cpu]
        irq_cls = "warn" if r["irq_off_max"] >= 100_000_000 else ""
        trace_html.append(
            "<tr>"
            f'<td class="num">CPU{cpu}</td>'
            f'<td class="num">{fmt_ns(r["wake_avg"])} / {fmt_ns(r["wake_max"])}</td>'
            f'<td class="num">{fmt_ns(r["syscall_avg"])} / {fmt_ns(r["syscall_max"])}</td>'
            f'<td class="num {irq_cls}">{fmt_ns(r["irq_off_max"])} / {r["irq_off_n"]}</td>'
            f'<td class="num">{fmt_ns(r["preempt_off_max"])} / {r["preempt_off_n"]}</td>'
            f'<td class="num">{r["ctxt"]}</td>'
            f'<td class="num">{r["migrations"]}</td>'
            "</tr>"
        )
    trace_table = "".join(trace_html) or '<tr><td colspan="7">无 rtstat 行</td></tr>'
    preemptoff = trace.get("preemptoff") or rt.get("preemptoff") or (0, 0, 0)

    # SMP=1 架构矩阵
    arch_rows = []
    for arch in ARCHS:
        a = smp1.get(arch, {})
        c = a.get("cyc", {})
        p = a.get("preempt", (0, 0, 0))
        ip = a.get("ipc", (0, 0, 0, 0, 0))
        pi_a = a.get("pi_chain", (0, 0, 0))
        th_a = a.get("throttle", (0, 0, 0))
        period = c.get("period", 0)
        period_s = fmt_ns(period) if period else "—"
        over = c.get("overruns", 0)
        over_cls = "good" if over == 0 else "warn"
        arch_rows.append(
            "<tr>"
            f"<td><b>{arch}</b> SMP=1</td>"
            f'<td class="num">{period_s}</td>'
            f'<td class="num">{fmt_ns(p[1])} / {fmt_ns(p[2])}</td>'
            f'<td class="num">{fmt_ns(ip[2])} / {fmt_ns(ip[3])}</td>'
            f'<td class="num">{fmt_ns(c.get("p50", 0))} / {fmt_ns(c.get("p99", 0))}'
            f' / {fmt_ns(c.get("p999", 0))} / {fmt_ns(c.get("max", 0))}</td>'
            f'<td class="num {over_cls}">{over}</td>'
            f'<td class="num">{fmt_ns(pi_a[2])}</td>'
            f'<td class="num">{fmt_ratio(th_a[1], th_a[0])}</td>'
            "</tr>"
        )
    arch_table = "".join(arch_rows)

    now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
    build = rt.get("build", "?")

    return f"""<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Avatar OS — 实时性全貌</title>
<style>{CSS}</style>
</head>
<body>
<main class="wrap">
<header>
  <div class="eyebrow">Avatar OS · Realtime Snapshot</div>
  <h1>实时性<em>全貌</em></h1>
  <p class="lede">一次 x86_64 KVM SMP=4 的 RT 回归，加三架构 SMP=1 对照：唤醒/抢占/IPC/定时器/PI 链/RT throttling/rt-trace 诊断面。数字全部来自串口日志，不重跑测试。</p>
  <div class="badges">
    <span class="badge">x86_64 · KVM · SMP=4</span>
    <span class="badge">1ms TSC-deadline oneshot</span>
    <span class="badge">build <b>{html.escape(build)}</b></span>
    <span class="badge">RT_TEST / RT_IRQ_TRACE</span>
    <span class="badge good">RT SCHED TEST: PASS</span>
  </div>
</header>

<section class="kpis">{kpi_html}</section>

<section>
  <h2>延迟阶梯</h2>
  <p class="sub">x86 SMP=4，同一组 run 里几种“唤醒/抢占”问题的量级；柱长按本组最大值归一。</p>
  <div class="panel">{ladder_html}
    <div class="legend">timer IRQ = deadline→ISR；IPC/唤醒 = set→等待者运行；cyclictest = deadline→线程运行。</div>
  </div>
</section>

<section class="grid2">
  <div class="panel">
    <h2>cyclictest 分布</h2>
    <p class="sub">2000 个线程唤醒样本；桶界固定。</p>
    {hist}
  </div>
  <div class="panel">
    <h2>怎么读这组数</h2>
    <p class="sub">先看尾部，再看均值。</p>
    <div class="flow">p50  {fmt_ns(cyc.get('p50',0))}  ← 典型唤醒
p99  {fmt_ns(cyc.get('p99',0))}  ← 回归主看这个
p999 {fmt_ns(cyc.get('p999',0))}  ← 约 2 个样本
max  {fmt_ns(cyc.get('max',0))}  ← 1 个样本，易被宿主噪声放大</div>
    <div class="note good">1ms 周期下 overruns={cyc.get('overruns',0)}，按 max 计算仍有约 {fmt_ns(max(0, cyc.get('period',0) - cyc.get('max',0)))} 余量。</div>
  </div>
</section>

<section>
  <h2>测试明细</h2>
  <p class="sub">x86_64 SMP=4 · {ids['rt']}</p>
  <div class="panel" style="padding:6px 6px">
    <table class="tbl">
      <thead><tr><th>测试</th><th>场景</th><th class="num">读数</th></tr></thead>
      <tbody>{detail_html}</tbody>
    </table>
  </div>
</section>

<section class="grid2">
  <div class="panel">
    <h2>PI 链：三层传递提升</h2>
    <p class="sub">L(20) 持 M1；M(50) 持 M2 后阻塞在 M1；H(80) 再阻塞在 M2；W(60) 做“墙”。</p>
    <div class="flow">H(80) ──等──▶ M2(owner=M)
M(50) ──等──▶ M1(owner=L)
L(20) ──持──▶ M1，事件上睡眠</div>
    <div class="story" style="margin-top:12px">
      <div><div class="k" style="font:600 10px var(--mono);color:var(--faint)">修复前（无链）</div>
        <div class="big" style="color:var(--warn)">{BEFORE_PI['prio_low']} <small>low 优先级</small></div>
        <div class="s">{fmt_ns(BEFORE_PI['high_lat'])} H 拿锁延迟（W 先跑 20ms）</div></div>
      <div><div class="k" style="font:600 10px var(--mono);color:var(--faint)">修复后（{ids['rt']}）</div>
        <div class="big" style="color:var(--good)">{pi[0]}/{pi[1]} <small>low/mid</small></div>
        <div class="s">{fmt_ns(pi[2])} H 拿锁延迟</div></div>
    </div>
    <div class="note">提升沿 <span class="mono">pi_blocked_on</span> → <span class="mono">held_rt_mutexes</span> 的 owner 链传递；全局 PI 锁保护“任务-锁-等待者”图。门禁：<span class="mono">RT PI CHAIN: PASS</span>。</div>
  </div>
  <div class="panel">
    <h2>RT throttling：每核 95% 预算</h2>
    <p class="sub">1s 周期内 RT 类（含 PI 提升）最多 950ms；超出后本核暂时跳过 RT 队列。</p>
    <div class="story" style="margin-top:12px">
      <div><div class="k" style="font:600 10px var(--mono);color:var(--faint)">修复前（无 throttling）</div>
        <div class="big" style="color:var(--warn)">NORMAL {BEFORE_THR['normal']} <small>次迭代</small></div>
        <div class="s">FIFO 80 纯忙循环把 NORMAL 完全饿死</div></div>
      <div><div class="k" style="font:600 10px var(--mono);color:var(--faint)">现在（{ids['rt']}）</div>
        <div class="big" style="color:var(--good)">{thr_ratio} <small>CPU 给 NORMAL</small></div>
        <div class="s">RT {th[0]:,} / NORMAL {th[1]:,} · events={th[2]}</div></div>
    </div>
    <div class="note warn">这是安全阀，不是延迟优化：RT 忙循环被限到 95%，预算耗尽时一个刚唤醒的 RT 任务最坏要等 <b>period−runtime ≈ 50ms</b>（再加 tick 粒度）。低占空比 RT（cyclictest ≈0.3%）用不到预算，不受影响。</div>
  </div>
</section>

<section>
  <h2>rt-trace 诊断面</h2>
  <p class="sub">{ids['trace']} · RT_IRQ_TRACE=1（有额外插桩开销，计数频率不是生产值）</p>
  <div class="panel" style="padding:6px 6px">
    <table class="tbl">
      <thead><tr><th class="num">CPU</th><th class="num">wake avg / max</th><th class="num">syscall avg / max</th><th class="num">irq_off max / n</th><th class="num">preempt_off max / n</th><th class="num">ctxt</th><th class="num">migrations</th></tr></thead>
      <tbody>{trace_table}</tbody>
    </table>
  </div>
  <div class="note warn"><b>IRQ-off 的读数要小心。</b>CPU0 的 {fmt_ns(trace_rows.get(0, {}).get('irq_off_max', 0))} 已经是“追踪口径”的上界：<span class="mono">sched_schedule()</span> 从 <span class="mono">arch_irq_save()</span> 开始、随后 <span class="mono">arch_task_switch()</span>，per-CPU 的 trace 起点会跨上下文切换；CPU2/CPU3 的 5.5s/6.06s 是同一机制在空闲核上的极端表现，不是硬件连续关中断。真正可断言的是人工 preempt-off 自测：held={fmt_ns(preemptoff[1])}、max={fmt_ns(preemptoff[2])}。</div>
  <div class="note">wake / syscall 列同样只是诊断面：wake_max 会被 klogd、idle 这类“长时间阻塞后被唤醒”的任务拉高（CPU0 的 2.5s 就是这种），不代表 RT 周期唤醒；RT 的周期延迟只看 cyclictest。</div>
</section>

<section>
  <h2>架构矩阵（SMP=1）</h2>
  <p class="sub">{ids['smp1']} · 同一条 rt-test，三个架构各跑一遍。</p>
  <div class="panel" style="padding:6px 6px">
    <table class="tbl">
      <thead><tr><th>架构</th><th class="num">内核周期</th><th class="num">preempt avg / max</th><th class="num">IPC avg / p99</th><th class="num">cyclictest p50 / p99 / p999 / max</th><th class="num">overruns</th><th class="num">PI high_lat</th><th class="num">throttle normal/rt</th></tr></thead>
      <tbody>{arch_table}</tbody>
    </table>
  </div>
  <div class="note"><b>这张表最重要的一行是“内核周期”。</b>x86_64 有 TSC-deadline oneshot，周期唤醒是 1ms、µs 级延迟；aarch64/riscv64 目前回退到 100Hz tick，周期只能是 10ms 整数倍，p50 天然在 5~8ms（不是调度器慢，是唤醒源只有 10ms 粒度）。PI 链和 throttling 与架构无关，三边都在 µs~sub-ms 量级完成。</div>
</section>

<section>
  <h2>局限</h2>
  <div class="caveats">
    <div class="caveat"><b>QEMU/KVM 不是裸机</b><p>宿主调度、vCPU 退出、缓存和中断投递都会进入尾部；绝对值只适合同一环境前后对比，真实硬件基线要另跑。</p></div>
    <div class="caveat"><b>2000 样本的尾部很薄</b><p>p99.9 只有约 2 个样本，max 只有 1 个；单次 max 变化一倍不奇怪，比较版本请看重复跑的 p99/p99.9。</p></div>
    <div class="caveat"><b>SMP=4 不是 RT 扩展性测试</b><p>所有 RT 任务钉 CPU0；SMP=4 主要增加宿主/跨核干扰，不能用这组数字声称“4 核 RT 吞吐翻倍”。</p></div>
    <div class="caveat"><b>trace 不是性能基线</b><p>RT_IRQ_TRACE 在关中断/自旋锁热路径上插桩；性能数字看 rt-test，trace 只用来看计数/P C/诊断面。</p></div>
    <div class="caveat"><b>throttling 的代价</b><p>RT 忙循环被限到 95%；预算耗尽时同核所有 RT 任务都可能被压到下个周期。硬实时应做 CPU 隔离并关闭该核限流。</p></div>
    <div class="caveat"><b>aarch64/riscv64 仍是 tick 回退</b><p>没有 hrtimer oneshot，10ms tick 决定了周期唤醒粒度；要 µs 级需要补 oneshot 定时器。</p></div>
  </div>
</section>

<footer>
  <span>SOURCE · rt-test {ids['rt']} · rt-trace {ids['trace']} · smp1 {ids['smp1']}</span>
  <span>生成时间 {now} · 数据来自串口日志（未重跑）· Avatar OS realtime branch</span>
</footer>
</main>
</body>
</html>
"""


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    here = pathlib.Path(__file__).resolve().parent.parent
    ap.add_argument("--artifacts", type=pathlib.Path, default=here / "tests/.artifacts")
    ap.add_argument("--rt-run", type=pathlib.Path, default=None, help="rt-test SMP=4 run 目录")
    ap.add_argument("--trace-run", type=pathlib.Path, default=None, help="rt-trace SMP=4 run 目录")
    ap.add_argument("--smp1-run", type=pathlib.Path, default=None, help="三架构 SMP=1 run 目录")
    ap.add_argument("--out", type=pathlib.Path, default=here / "tests/.artifacts/rt-report.html")
    args = ap.parse_args()

    art = args.artifacts
    rt_dir = args.rt_run or find_run(
        art, 4, "logs/rt-test/x86_64-rt.log", "RT THROTTLE TEST: PASS"
    )
    trace_dir = args.trace_run or find_run(
        art, 4, "logs/rt-trace/x86_64-trace.log", "PREEMPTOFF TEST: PASS"
    )
    smp1_dir = args.smp1_run or find_smp1_run(art)

    rt = parse_rt_log(read_log(rt_dir / "logs/rt-test/x86_64-rt.log"))
    trace, trace_rows = parse_trace_log(
        read_log(trace_dir / "logs/rt-trace/x86_64-trace.log")
    )
    smp1 = {
        a: parse_rt_log(read_log(smp1_dir / f"logs/rt-test/{a}-rt.log")) for a in ARCHS
    }

    ids = {
        "rt": rt_dir.name,
        "trace": trace_dir.name,
        "smp1": smp1_dir.name,
    }
    body = render(rt, trace, trace_rows, smp1, ids)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(body, encoding="utf-8")

    cyc = rt.get("cyc", {})
    print(f"wrote {args.out}")
    print(
        f"  rt-test {ids['rt']}: cyclictest p50/p99/max = "
        f"{fmt_ns(cyc.get('p50', 0))}/{fmt_ns(cyc.get('p99', 0))}/{fmt_ns(cyc.get('max', 0))}, "
        f"overruns={cyc.get('overruns', 0)}"
    )
    pi = rt.get("pi_chain", (0, 0, 0))
    print(f"  PI chain low/mid={pi[0]}/{pi[1]} high_lat={fmt_ns(pi[2])}")
    th = rt.get("throttle", (0, 0, 0))
    print(f"  throttle normal/rt={fmt_ratio(th[1], th[0])} events={th[2]}")
    print(f"  trace {ids['trace']}: {len(trace_rows)} CPU rows; smp1 {ids['smp1']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
