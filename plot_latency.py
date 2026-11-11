#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""
Plot Ctrl-C delivery latency distributions from latency_mode*.csv files.

Usage:
    python3 plot_latency.py [dir]

Scans <dir> (default: .) for latency_mode<N>_<timestamp>.csv files, groups
them by mode (0 = signal, 1 = UINTR), and renders four figures:
  latency_cdf_<ts>.png          -- cumulative distribution
  latency_hist_<ts>.png         -- log-log histogram
  latency_violin_<ts>.png       -- violin plot per run
  latency_strip_<ts>.png        -- jittered strip plot (sampled)
"""
import glob
import os
import re
import sys
import time
from datetime import datetime

import numpy as np
from PIL import Image, ImageDraw, ImageFont

FONT_REG = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
FONT_BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"

MODE_COLORS = {0: "#2F7FC1", 1: "#D95F02"}
MODE_LABEL = {0: "SIGINT", 1: "UINTR "}
RUN_COLORS = ["#2F7FC1", "#D95F02", "#7570B3", "#1B9E77", "#E7298A", "#E6AB02"]


def load_fonts():
    try:
        f = ImageFont.truetype(FONT_REG, 15)
        fb = ImageFont.truetype(FONT_BOLD, 19)
        fs = ImageFont.truetype(FONT_REG, 13)
        fl = ImageFont.truetype(FONT_BOLD, 14)
    except Exception:
        d = ImageFont.load_default()
        f = fb = fs = fl = d
    return f, fb, fs, fl


def find_csvs(directory):
    """Return dict mode -> list of (path, label) sorted by mtime."""
    groups = {}
    pat = re.compile(r"latency_mode(\d+)(?:_(\d+_\d+))?\.csv$")
    files = glob.glob(os.path.join(directory, "latency_mode*.csv"))
    for p in sorted(files):
        m = pat.match(os.path.basename(p))
        if not m:
            continue
        mode = int(m.group(1))
        ts = m.group(2) or datetime.fromtimestamp(os.path.getmtime(p)).strftime("%Y%m%d_%H%M%S")
        d = np.loadtxt(p, skiprows=1)
        if d.size == 0:
            continue
        groups.setdefault(mode, []).append((p, d, ts))
    for v in groups.values():
        v.sort(key=lambda x: x[2])
    return groups


def stats(d):
    return {
        "min": d.min() / 1000,
        "p50": np.percentile(d, 50) / 1000,
        "p90": np.percentile(d, 90) / 1000,
        "p99": np.percentile(d, 99) / 1000,
        "max": d.max() / 1000,
    }


# ---------- CDF ----------
def plot_cdf(groups, out, f, fb, fs, fl):
    W, H = 1300, 800
    ML, MR, MT, MB = 100, 45, 135, 95
    pw, ph = W - ML - MR, H - MT - MB
    LO, HI = 1e3, 4e6

    def xpos(v):
        return ML + (np.log10(v) - np.log10(LO)) / (np.log10(HI) - np.log10(LO)) * pw

    def ypos(p):
        return MT + ph - p * ph

    img = Image.new("RGB", (W, H), "#ffffff")
    dr = ImageDraw.Draw(img)

    for p in [0, 0.25, 0.5, 0.75, 0.9, 0.99, 1]:
        y = ypos(p)
        dr.line([(ML, y), (ML + pw, y)], fill="#e8e8e8")
        dr.text((ML - 10, y - 7), f"{int(p*100)}%", anchor="ra", font=fs, fill="#777777")
    for v, lab in [(1e3, "1 us"), (1e4, "10 us"), (1e5, "100 us"), (1e6, "1 ms")]:
        x = xpos(v)
        dr.line([(x, MT), (x, MT + ph)], fill="#e8e8e8")
        dr.text((x, MT + ph + 12), lab, anchor="ma", font=f, fill="#333333")

    runs = []
    for mode, items in groups.items():
        col = MODE_COLORS.get(mode, "#333333")
        for i, (p, d, ts) in enumerate(items):
            runs.append((d, col, f"{MODE_LABEL.get(mode,'?')} {ts}"))

    for d, col, lab in runs:
        xs = np.sort(d)
        n = len(xs)
        ys = (np.arange(n) + 1) / n
        pts = [(xpos(x), ypos(y)) for x, y in zip(xs, ys)]
        dr.line(pts, fill=col, width=2)

    dr.text((ML, 18), "CDF: Ctrl-C delivery latency (async workqueue path)", font=fb, fill="#111111")
    n = len(runs[0][0]) if runs else 0
    dr.text((ML, 46), f"{len(runs)} runs, n={n} each, receiver pinned + SCHED_FIFO", font=f, fill="#666666")

    y = 76
    for d, col, lab in runs:
        s = stats(d)
        slow = ((d >= 64000) & (d < 128000)).sum() / len(d) * 100
        dr.rectangle([ML, y + 3, ML + 18, y + 9], fill=col)
        dr.text((ML + 28, y - 2),
                f"{lab}:  p50 {s['p50']:.2f}  p90 {s['p90']:.2f}  p99 {s['p99']:.2f} us   [64,128)us {slow:.1f}%",
                font=fl, fill=col)
        y += 21

    dr.rectangle([ML, MT, ML + pw, MT + ph], outline="#333333")
    dr.text((ML + pw / 2, H - 34), "latency (log scale)", anchor="ma", font=f, fill="#333333")
    dr.text((26, MT + ph / 2), "cumulative probability", font=f, fill="#333333")
    img.save(out)
    print(f"[OK] {out}")


# ---------- histogram ----------
def plot_hist(groups, out, f, fb, fs, fl):
    W, H = 1300, 800
    ML, MR, MT, MB = 100, 45, 135, 95
    pw, ph = W - ML - MR, H - MT - MB
    LO, HI = 1e3, 4e6
    bins = np.append(LO * 2.0 ** (np.arange(0, 27) * 0.5), np.inf)

    def xpos(v):
        return ML + (np.log10(v) - np.log10(LO)) / (np.log10(HI) - np.log10(LO)) * pw

    runs = []
    for mode, items in groups.items():
        col = MODE_COLORS.get(mode, "#333333")
        for p, d, ts in items:
            runs.append((d, col, f"{MODE_LABEL.get(mode,'?')} {ts}"))

    hist = [np.histogram(d, bins=bins)[0] for d, _, _ in runs]
    peak = max(h.max() for h in hist)
    ymax = max(np.log10(peak + 1), 1.0)

    def ypos(c):
        return MT + ph - (np.log10(c + 1) / ymax) * ph

    img = Image.new("RGB", (W, H), "#ffffff")
    dr = ImageDraw.Draw(img)

    for e in range(0, int(ymax) + 1):
        y = MT + ph - e / ymax * ph
        dr.line([(ML, y), (ML + pw, y)], fill="#e8e8e8")
        dr.text((ML - 10, y - 7), f"10^{e}", anchor="ra", font=fs, fill="#777777")
    for v, lab in [(1e3, "1 us"), (1e4, "10 us"), (1e5, "100 us"), (1e6, "1 ms")]:
        x = xpos(v)
        dr.line([(x, MT), (x, MT + ph)], fill="#e8e8e8")
        dr.text((x, MT + ph + 12), lab, anchor="ma", font=f, fill="#333333")

    centers = []
    for i in range(len(bins) - 1):
        hi = HI * 1.2 if bins[i + 1] == np.inf else bins[i + 1]
        centers.append(np.sqrt(bins[i] * hi))

    for h, (_, col, _l) in zip(hist, runs):
        pts = [(xpos(c), ypos(v)) for c, v in zip(centers, h)]
        dr.line(pts, fill=col, width=3, joint="curve")
        for x, y in pts:
            dr.ellipse([x - 4, y - 4, x + 4, y + 4], fill=col)

    dr.text((ML, 18), "Histogram: Ctrl-C delivery latency (log-log)", font=fb, fill="#111111")
    dr.text((ML, 46), f"{len(runs)} runs, log2-spaced bins", font=f, fill="#666666")

    y = 76
    for d, col, lab in runs:
        s = stats(d)
        dr.rectangle([ML, y + 3, ML + 18, y + 9], fill=col)
        dr.text((ML + 28, y - 2),
                f"{lab}:  min {s['min']:.2f}  p50 {s['p50']:.2f}  p90 {s['p90']:.2f}  max {s['max']:.1f} us",
                font=fl, fill=col)
        y += 21

    dr.rectangle([ML, MT, ML + pw, MT + ph], outline="#333333")
    dr.text((ML + pw / 2, H - 34), "latency (log scale)", anchor="ma", font=f, fill="#333333")
    dr.text((26, MT + ph / 2), "count (log)", font=f, fill="#333333")
    img.save(out)
    print(f"[OK] {out}")


# ---------- violin ----------
def plot_violin(groups, out, f, fb, fs):
    runs = []
    for mode, items in groups.items():
        col = MODE_COLORS.get(mode, "#333333")
        for p, d, ts in items:
            runs.append((d, col, f"{MODE_LABEL.get(mode,'?')}\n{ts}"))

    W, H = 1300, 800
    ML, MR, MT, MB = 110, 45, 75, 100
    pw, ph = W - ML - MR, H - MT - MB
    LO, HI = 1e3, 4e6

    def ypos(v):
        return MT + ph - (np.log10(v) - np.log10(LO)) / (np.log10(HI) - np.log10(LO)) * ph

    img = Image.new("RGB", (W, H), "#ffffff")
    dr = ImageDraw.Draw(img)

    for v, lab in [(1e3, "1 us"), (1e4, "10 us"), (1e5, "100 us"), (1e6, "1 ms")]:
        y = ypos(v)
        dr.line([(ML, y), (ML + pw, y)], fill="#e8e8e8")
        dr.text((ML - 10, y - 7), lab, anchor="ra", font=fs, fill="#777777")

    n_groups = len(runs)
    slot = pw / max(n_groups, 1)
    half_w = slot * 0.32

    for i, (d, col, lab) in enumerate(runs):
        cx = ML + slot * (i + 0.5)
        v = np.sort(np.log10(d))
        edges = np.linspace(np.log10(LO), np.log10(HI), 61)
        centers = (edges[:-1] + edges[1:]) / 2
        counts = np.histogram(v, bins=edges)[0]
        if counts.max() > 0:
            counts = counts / counts.max()
        pts = []
        for c, dn in zip(centers, counts):
            pts.append((cx - half_w * dn, ypos(10 ** c)))
        for c, dn in zip(reversed(centers), reversed(counts)):
            pts.append((cx + half_w * dn, ypos(10 ** c)))
        dr.polygon(pts, fill=col, outline=col)
        my = ypos(np.median(d))
        dr.line([(cx - half_w, my), (cx + half_w, my)], fill="#ffffff", width=3)
        for j, line in enumerate(lab.split("\n")):
            dr.text((cx, MT + ph + 14 + j * 16), line, anchor="ma", font=fs, fill="#333333")

    dr.text((ML, 22), "Violin: Ctrl-C delivery latency by run", font=fb, fill="#111111")
    dr.text((ML, 50), "white line = median, x = log(latency)", font=f, fill="#666666")
    dr.rectangle([ML, MT, ML + pw, MT + ph], outline="#333333")
    dr.text((28, MT + ph / 2), "latency (log scale)", font=f, fill="#333333")
    img.save(out)
    print(f"[OK] {out}")


# ---------- strip ----------
def plot_strip(groups, out, f, fb, fs):
    rng = np.random.default_rng(42)
    runs = []
    for mode, items in groups.items():
        col = MODE_COLORS.get(mode, "#333333")
        for p, d, ts in items:
            idx = rng.choice(len(d), size=min(2000, len(d)), replace=False)
            runs.append((d[idx], col, f"{MODE_LABEL.get(mode,'?')}\n{ts}"))

    W, H = 1300, 800
    ML, MR, MT, MB = 110, 45, 75, 100
    pw, ph = W - ML - MR, H - MT - MB
    LO, HI = 1e3, 4e6

    def ypos(v):
        return MT + ph - (np.log10(v) - np.log10(LO)) / (np.log10(HI) - np.log10(LO)) * ph

    img = Image.new("RGB", (W, H), "#ffffff")
    dr = ImageDraw.Draw(img)

    for v, lab in [(1e3, "1 us"), (1e4, "10 us"), (1e5, "100 us"), (1e6, "1 ms")]:
        y = ypos(v)
        dr.line([(ML, y), (ML + pw, y)], fill="#e8e8e8")
        dr.text((ML - 10, y - 7), lab, anchor="ra", font=fs, fill="#777777")

    n_groups = len(runs)
    slot = pw / max(n_groups, 1)
    jitter = slot * 0.35

    for i, (d, col, lab) in enumerate(runs):
        cx = ML + slot * (i + 0.5)
        for val in d:
            ox = cx + (rng.random() - 0.5) * jitter
            dr.ellipse([ox - 2, ypos(val) - 2, ox + 2, ypos(val) + 2], fill=col)
        my = ypos(np.median(d))
        dr.line([(cx - slot * 0.3, my), (cx + slot * 0.3, my)], fill="#000000", width=2)
        for j, line in enumerate(lab.split("\n")):
            dr.text((cx, MT + ph + 14 + j * 16), line, anchor="ma", font=fs, fill="#333333")

    dr.text((ML, 22), "Strip (jittered): Ctrl-C delivery latency by run", font=fb, fill="#111111")
    dr.text((ML, 50), "2000 samples each, black line = median", font=f, fill="#666666")
    dr.rectangle([ML, MT, ML + pw, MT + ph], outline="#333333")
    dr.text((28, MT + ph / 2), "latency (log scale)", font=f, fill="#333333")
    img.save(out)
    print(f"[OK] {out}")


def main():
    directory = sys.argv[1] if len(sys.argv) > 1 else "."
    groups = find_csvs(directory)
    if not groups:
        print(f"[FAIL] no latency_mode*.csv found in {directory}")
        sys.exit(1)

    total = sum(len(v) for v in groups.values())
    print(f"[INFO] found {total} run(s) across modes {sorted(groups)}")
    for mode, items in groups.items():
        for p, d, ts in items:
            s = stats(d)
            print(f"  mode={mode} {ts}: n={len(d)} p50={s['p50']:.2f}us p90={s['p90']:.2f}us")

    ts = time.strftime("%Y%m%d_%H%M%S")
    f, fb, fs, fl = load_fonts()
    plot_cdf(groups, os.path.join(directory, f"latency_cdf_{ts}.png"), f, fb, fs, fl)
    plot_hist(groups, os.path.join(directory, f"latency_hist_{ts}.png"), f, fb, fs, fl)
    plot_violin(groups, os.path.join(directory, f"latency_violin_{ts}.png"), f, fb, fs)
    plot_strip(groups, os.path.join(directory, f"latency_strip_{ts}.png"), f, fb, fs)


if __name__ == "__main__":
    main()
