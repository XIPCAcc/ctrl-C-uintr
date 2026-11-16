#!/usr/bin/env python3
import glob
import os

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

ROOT = os.path.dirname(os.path.abspath(__file__))


def latest(stem):
    """Newest CSV for a mode, plain name or timestamped."""
    hits = sorted(glob.glob(os.path.join(ROOT, stem + "*.csv")),
                  key=os.path.getmtime)
    if not hits:
        raise SystemExit(f"no CSV found for {stem}")
    return hits[-1]


data = {}
for mode in (0, 1):
    path = latest(f"freq_latency_mode{mode}")
    print("using", path)
    raw = np.genfromtxt(path, delimiter=",", names=True, dtype=np.int64)
    data[mode] = np.atleast_1d(raw)

periods = sorted(set(data[0]["period_us"]) | set(data[1]["period_us"]))
values = {
    period: {
        mode: data[mode]["latency_ns"][data[mode]["period_us"] == period] / 1000.0
        for mode in (0, 1)
    }
    for period in periods
}
combined = np.concatenate([values[p][m] for p in periods for m in (0, 1)])
start = np.floor(np.log2(combined.min()) * 2) / 2
stop = np.ceil(np.log2(combined.max()) * 2) / 2
bins = 2.0 ** np.arange(start, stop + 0.5, 0.5)
centers = np.sqrt(bins[:-1] * bins[1:])
histograms = {
    (period, mode): np.histogram(values[period][mode], bins=bins)[0]
    for period in periods for mode in (0, 1)
}
ymax = max(counts.max() for counts in histograms.values()) * 1.4

for period in periods:
    fig, ax = plt.subplots(figsize=(13, 8))
    for mode, color, label in ((0, "#2F7FC1", "SIGINT"), (1, "#D95F02", "UINTR")):
        counts = histograms[(period, mode)]
        visible = counts > 0
        ax.plot(centers[visible], counts[visible], color=color, marker="o",
                linewidth=2.2, markersize=4.5,
                label=f"{label}  n={len(values[period][mode])}")

    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlim(bins[0], bins[-1])
    ax.set_ylim(0.8, ymax)
    ax.set_xlabel("latency (us, log scale)")
    ax.set_ylabel("sample count (log scale)")
    ax.set_title(f"SIGINT vs UINTR latency, period={period} us "
                 f"(~{1000 / period:.0f} kHz)",
                 fontsize=16, fontweight="bold", loc="left")
    ax.grid(which="both", alpha=0.25)
    ax.legend(loc="upper left")
    fig.tight_layout()
    out = os.path.join(ROOT, f"latency_hist_pair_{period}us.png")
    fig.savefig(out, dpi=160)
    plt.close(fig)
    print(out)
