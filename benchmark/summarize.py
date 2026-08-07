#!/usr/bin/env python3
"""Summarize bench-suite.sh results: median per cell, delta vs config A.

Usage: summarize.py results.csv
The first run of every cell is discarded as warmup (when more than one).
For the distinct_10k workload the value is seconds per pass (lower is
better); everything else is tps (higher is better).
"""
import statistics
import sys

data = {}
failed = {}
with open(sys.argv[1]) as f:
    header = f.readline()
    for line in f:
        cfg, wl, run, val = line.strip().split(',')
        if not val:
            # pgbench aborted before reporting: count it, don't crash
            failed[(wl, cfg)] = failed.get((wl, cfg), 0) + 1
            continue
        data.setdefault((wl, cfg), []).append(float(val))

workloads = []
configs = []
for (wl, cfg) in data:
    if wl not in workloads:
        workloads.append(wl)
    if cfg not in configs:
        configs.append(cfg)
configs.sort()

def median(vals):
    vals = vals[1:] if len(vals) > 1 else vals   # drop warmup run
    return statistics.median(vals)

print(f"{'workload':<14}", end='')
for cfg in configs:
    print(f" | {cfg:>21}", end='')
print()

for wl in workloads:
    if ('A' not in [c for (w, c) in data if w == wl]):
        continue
    base = median(data[(wl, 'A')])
    lower_is_better = wl.startswith('distinct')
    row = f"{wl:<14}"
    for cfg in configs:
        vals = data.get((wl, cfg))
        if not vals:
            row += f" | {'-':>21}"
            continue
        m = median(vals)
        delta = (m / base - 1) * 100
        if lower_is_better:
            delta = -delta
        row += f" | {m:>12.1f} ({delta:+5.1f}%)"
    print(row)

print("\n(distinct_10k is seconds/pass, lower is better; delta sign is "
      "normalized so negative always means 'worse than baseline A')")

if failed:
    print("\nFAILED RUNS (pgbench aborted before reporting):")
    for (wl, cfg), n in sorted(failed.items()):
        print(f"  {cfg}/{wl}: {n} run(s)")
