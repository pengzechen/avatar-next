#!/usr/bin/env python3
"""
plot_cyclictest.py — 把一次 CYCLICTEST 结果画成一张汇总图

用法示例（数值直接来自串口 CYCLICTEST 行）：
  python3 tools/plot_cyclictest.py \
      --period-ns 1000000 --warmup 100 --samples 2000 \
      --min-ns 2338 --avg-ns 2503 --p50-ns 2397 \
      --p99-ns 4575 --p999-ns 10814 --max-ns 12689 \
      --overruns 0 --early 0 \
      --out docs/realtime/cyclictest_1ms.png

  # 深色版本：
  python3 tools/plot_cyclictest.py ... --dark \
      --out docs/realtime/cyclictest_1ms_dark.png

依赖宿主 matplotlib（不是内核工具）。图里只画**已知的汇总值**，不伪造
直方图/原始样本；上半部分是百分位梯子，下半部分是相对 1ms 周期的预算条。
"""

from __future__ import annotations

import argparse

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch

# 参考调色板（dataviz skill 的 validated default palette）。
LIGHT = {
    "page": "#f9f9f7",
    "surface": "#fcfcfb",
    "ink": "#0b0b0b",
    "secondary": "#52514e",
    "muted": "#898781",
    "grid": "#e1e0d9",
    "baseline": "#c3c2b7",
    "border": "#deddd6",
    "accent": "#2a78d6",
    "accent_soft": "#86b6ef",
    "track": "#e7e6df",
    "good": "#006300",
}
DARK = {
    "page": "#0d0d0d",
    "surface": "#1a1a19",
    "ink": "#ffffff",
    "secondary": "#c3c2b7",
    "muted": "#898781",
    "grid": "#2c2c2a",
    "baseline": "#383835",
    "border": "#383835",
    "accent": "#3987e5",
    "accent_soft": "#6da7ec",
    "track": "#2c2c2a",
    "good": "#0ca30c",
}


def us(ns: int) -> float:
    return ns / 1000.0


def pct(ns: int, period_ns: int) -> float:
    return 100.0 * ns / period_ns


def rounded_card(fig, x, y, w, h, face, edge):
    card = FancyBboxPatch(
        (x, y),
        w,
        h,
        boxstyle="round,pad=0.006,rounding_size=0.012",
        transform=fig.transFigure,
        facecolor=face,
        edgecolor=edge,
        linewidth=1.0,
        zorder=1,
    )
    fig.add_artist(card)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--period-ns", type=int, required=True)
    ap.add_argument("--warmup", type=int, default=0)
    ap.add_argument("--samples", type=int, required=True)
    ap.add_argument("--min-ns", type=int, required=True)
    ap.add_argument("--avg-ns", type=int, required=True)
    ap.add_argument("--p50-ns", type=int, required=True)
    ap.add_argument("--p99-ns", type=int, required=True)
    ap.add_argument("--p999-ns", type=int, required=True)
    ap.add_argument("--max-ns", type=int, required=True)
    ap.add_argument("--overruns", type=int, default=0)
    ap.add_argument("--early", type=int, default=0)
    ap.add_argument("--out", default="docs/realtime/cyclictest_1ms.png")
    ap.add_argument("--dark", action="store_true")
    ap.add_argument("--title", default="1ms 周期下的唤醒延迟")
    ap.add_argument("--subtitle", default="cyclictest · x86_64 KVM · FIFO 80 + NORMAL hog")
    ap.add_argument(
        "--scenario",
        default="SMP=1 · CPU0 被 1 个 NORMAL 忙循环占满 · 网络/磁盘/用户进程未启动",
    )
    args = ap.parse_args()

    c = DARK if args.dark else LIGHT
    plt.rcParams.update(
        {
            "font.family": ["Noto Sans CJK JP", "Droid Sans Fallback", "DejaVu Sans"],
            "font.size": 11,
            "axes.unicode_minus": False,
            "figure.facecolor": c["page"],
            "savefig.facecolor": c["page"],
        }
    )

    period = args.period_ns
    pts = [
        ("min", args.min_ns),
        ("p50", args.p50_ns),
        ("p99", args.p99_ns),
        ("p99.9", args.p999_ns),
        ("max", args.max_ns),
    ]

    fig = plt.figure(figsize=(14, 8.5))

    # ── 标题 ────────────────────────────────────────────────────────
    fig.text(0.06, 0.935, "AVATAR OS · REALTIME", color=c["muted"],
             fontsize=10, fontweight="bold")
    fig.text(0.06, 0.875, args.title, color=c["ink"], fontsize=27,
             fontweight="bold")
    fig.text(
        0.06,
        0.832,
        f"{args.subtitle} · {args.samples} 次采样 · overrun {args.overruns}",
        color=c["secondary"],
        fontsize=12,
    )

    # ── 右上角三张卡 ────────────────────────────────────────────────
    cards = [
        ("最坏延迟", f"{us(args.max_ns):.2f} µs", f"占 1ms 的 {pct(args.max_ns, period):.2f}%"),
        ("p99", f"{us(args.p99_ns):.2f} µs", f"占 1ms 的 {pct(args.p99_ns, period):.2f}%"),
        ("overrun", f"{args.overruns} / {args.samples}", "早醒 " + str(args.early)),
    ]
    card_x = [0.585, 0.725, 0.865]
    for x, (label, value, sub) in zip(card_x, cards):
        rounded_card(fig, x, 0.755, 0.125, 0.135, c["surface"], c["border"])
        fig.text(x + 0.012, 0.861, label, color=c["muted"], fontsize=9.5, zorder=10)
        fig.text(x + 0.012, 0.812, value, color=c["ink"], fontsize=19,
                 fontweight="bold", zorder=10)
        fig.text(x + 0.012, 0.778, sub, color=c["secondary"], fontsize=8.8, zorder=10)

    # ── 上半：百分位梯子 ────────────────────────────────────────────
    ax = fig.add_axes([0.145, 0.405, 0.79, 0.295])
    ax.set_facecolor(c["surface"])
    ax.set_xlim(0, 14)
    ax.set_ylim(-0.65, 4.65)
    ax.grid(axis="x", color=c["grid"], linewidth=0.8, linestyle="-", zorder=0)
    ax.set_axisbelow(True)
    for spine in ("top", "right", "left"):
        ax.spines[spine].set_visible(False)
    ax.spines["bottom"].set_color(c["baseline"])
    ax.tick_params(axis="x", colors=c["muted"], labelsize=10, length=0)
    ax.tick_params(axis="y", colors=c["secondary"], labelsize=11, length=0)
    ax.set_xticks([0, 2, 4, 6, 8, 10, 12, 14])
    ax.set_yticks(range(len(pts)))
    ax.set_yticklabels([name for name, _ in pts])
    ax.set_xlabel("唤醒延迟（µs，相对绝对 deadline；越靠左越好）",
                  color=c["secondary"], fontsize=10.5, labelpad=8, loc="left")

    for y, (name, val) in enumerate(pts):
        x = us(val)
        ax.hlines(y, 0, x, color=c["accent_soft"], linewidth=2.4, zorder=2)
        ax.scatter([x], [y], s=95, color=c["accent"], edgecolor=c["surface"],
                   linewidth=2.0, zorder=3)
        weight = "bold" if name == "max" else "normal"
        ax.text(x + 0.35, y, f"{us(val):.2f} µs", va="center",
                color=c["ink"], fontsize=11, fontweight=weight)

    ax.text(0.0, 1.035, "唤醒延迟分布（min → max）", transform=ax.transAxes,
            color=c["ink"], fontsize=13, fontweight="bold")
    ax.text(1.0, 1.035, f"p99 {us(args.p99_ns):.2f} µs · p99.9 {us(args.p999_ns):.2f} µs",
            transform=ax.transAxes, ha="right", color=c["secondary"], fontsize=10)

    # ── 下半：1ms 预算条 ────────────────────────────────────────────
    ax2 = fig.add_axes([0.145, 0.185, 0.79, 0.085])
    ax2.set_xlim(0, period)
    ax2.set_ylim(0, 1)
    ax2.axis("off")
    ax2.plot([0, period], [0.35, 0.35], color=c["track"], linewidth=16,
             solid_capstyle="round", zorder=1)
    ax2.plot([0, args.max_ns], [0.35, 0.35], color=c["accent"], linewidth=16,
             solid_capstyle="butt", zorder=2)

    # 最坏延迟的指针：放大标注，说明它在 1ms 里只是一小段
    ax2.annotate(
        f"最坏 {us(args.max_ns):.2f} µs = {pct(args.max_ns, period):.2f}%",
        xy=(args.max_ns, 0.35),
        xytext=(args.max_ns + 80_000, 0.78),
        color=c["ink"],
        fontsize=10.5,
        fontweight="bold",
        ha="left",
        va="center",
        arrowprops=dict(arrowstyle="-", color=c["muted"], linewidth=1.0),
    )
    ax2.text(period * 0.52, 0.35, f"剩余 {us(period - args.max_ns):.1f} µs（{100 - pct(args.max_ns, period):.2f}%）",
             ha="center", va="center", color=c["muted"], fontsize=10)
    ax2.text(0, 0.02, "0", ha="center", va="bottom", color=c["muted"], fontsize=9)
    ax2.text(period, 0.02, "1 ms", ha="right", va="bottom", color=c["muted"], fontsize=9)
    ax2.text(0.0, 1.22, "相对 1ms 周期：最坏延迟只占 "
             f"{pct(args.max_ns, period):.2f}%", transform=ax2.transAxes,
             color=c["ink"], fontsize=13, fontweight="bold")
    ax2.text(1.0, 1.22,
             f"p99 {pct(args.p99_ns, period):.2f}% · p99.9 {pct(args.p999_ns, period):.2f}%",
             transform=ax2.transAxes, ha="right", color=c["secondary"], fontsize=10)

    # ── 页脚 ────────────────────────────────────────────────────────
    fig.text(0.06, 0.118,
             "场景：" + args.scenario,
             color=c["ink"], fontsize=9.8, fontweight="bold")
    fig.text(0.06, 0.095,
             "延迟 = 实际醒来时间 − 绝对 deadline；min/avg/p50/p99/p99.9/max = "
             f"{us(args.min_ns):.2f}/{us(args.avg_ns):.2f}/{us(args.p50_ns):.2f}/"
             f"{us(args.p99_ns):.2f}/{us(args.p999_ns):.2f}/{us(args.max_ns):.2f} µs",
             color=c["secondary"], fontsize=9.5)
    fig.text(0.06, 0.062,
             "QEMU/KVM 单次运行，不是裸机保证；内核 RT 线程，不含 syscall/页错误噪声。",
             color=c["muted"], fontsize=9.5)
    fig.text(0.06, 0.038,
             f"period={period} ns · warmup={args.warmup} · samples={args.samples} · "
             f"overruns={args.overruns} · early={args.early}",
             color=c["muted"], fontsize=9)

    fig.savefig(args.out, dpi=180, facecolor=c["page"])
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
