#!/usr/bin/env python3
"""Applies the L1 decision rule (CONTEXT.md P79) to layout_bench CSV output.

Usage: analyze.py results.csv [more.csv ...]

Rows: backend,gpu,mode,res,workload,pass,median_ms,p10_ms,p90_ms. Repeated runs of the
same configuration are averaged (median of medians). For every backend/GPU/resolution
and every non-baseline mode, compared with the baseline, as P79 states it:
  (1) the realistic frame (workload "frame", row "frame") at most 2% slower;
  (2) every pass of every workload (and the total of each smaller workload) at most
      10% slower, unless the difference is under 0.05 ms.
A mode passes on a GPU when both hold. Prints Markdown.
"""

import csv
import math
import statistics
import sys
from collections import defaultdict

FRAME_LIMIT = 0.02
PASS_LIMIT = 0.10
NOISE_MS = 0.05


def load(paths):
    runs = defaultdict(list)  # (backend, gpu, mode, res, workload, pass) -> [median, ...]
    for path in paths:
        with open(path, newline="") as f:
            for row in csv.reader(f):
                if len(row) != 9 or row[0] == "backend":
                    continue
                backend, gpu, mode, res, workload, pass_, median, _p10, _p90 = row
                runs[(backend, gpu, mode, res, workload, pass_)].append(float(median))
    return {k: statistics.median(v) for k, v in runs.items()}, {k: v for k, v in runs.items()}


def balance(groups):
    """Placement-balanced runs (--swap, harness v5). On the AMD iGPU the place of a
    texture copy in memory alone moved passes by up to 18%, whichever path used it.
    Each interleaved run compares RI on one copy with the baseline on the other;
    --swap on reverses the copies. With a placement factor p0, p1:
        off: RI/baseline = (a p0)/(b p1),  on: RI/baseline = (a p1)/(b p0),
    so the geometric mean of the two ratios is a/b, free of placement, and each ratio
    comes from one interleaved run (same clock state). The balanced mode
    "<mode>[balanced]" carries baseline = geometric mean of the two baselines and
    A' = baseline x ratio; the separate off/on modes are not judged."""
    for modes in groups.values():
        for mode in [m for m in modes if not m.startswith("baseline") and not m.endswith("[swap]")]:
            needed = [mode + "[swap]", "baseline:" + mode, "baseline:" + mode + "[swap]"]
            if not all(n in modes for n in needed):
                continue
            off, on = modes.pop(mode), modes.pop(mode + "[swap]")
            base_off, base_on = modes.pop("baseline:" + mode), modes.pop("baseline:" + mode + "[swap]")
            base, values = {}, {}
            for key in base_off:
                if key in base_on and key in off and key in on and base_off[key] > 0 and base_on[key] > 0:
                    ratio = math.sqrt((off[key] / base_off[key]) * (on[key] / base_on[key]))
                    base[key] = math.sqrt(base_off[key] * base_on[key])
                    values[key] = base[key] * ratio
            modes[mode + "[balanced]"] = values
            modes["baseline:" + mode + "[balanced]"] = base


def main():
    medians, raw = load(sys.argv[1:])
    groups = defaultdict(lambda: defaultdict(dict))  # (backend,gpu,res) -> mode -> {(workload,pass): ms}
    for (backend, gpu, mode, res, workload, pass_), ms in medians.items():
        groups[(backend, gpu, res)][mode][(workload, pass_)] = ms
    balance(groups)

    verdicts = []
    for (backend, gpu, res), modes in sorted(groups.items()):
        for mode, values in sorted(modes.items()):
            if mode.startswith("baseline"):
                continue
            # Interleaved runs (--interleave on) carry their own baseline,
            # "baseline:<mode>", measured frame by frame alongside the mode.
            base = modes.get("baseline:" + mode, modes.get("baseline"))
            if base is None:
                continue
            print(f"\n### {backend}, {gpu}, {res}p: `{mode}` vs baseline\n")
            print("| Workload | Pass | Baseline ms | A' ms | Diff ms | Diff % | OK |")
            print("|---|---|---:|---:|---:|---:|:-:|")
            failures = []
            for (workload, pass_), b in sorted(base.items()):
                if (workload, pass_) not in values or pass_ in ("cpu_submit_wait", "between"):
                    continue  # CPU wall time and between-pass time: reported, not P79 passes
                a = values[(workload, pass_)]
                diff = a - b
                pct = diff / b if b > 0 else 0.0
                if workload == "frame" and pass_ == "frame":
                    ok = pct <= FRAME_LIMIT  # rule (1): the realistic frame
                else:
                    ok = pct <= PASS_LIMIT or diff < NOISE_MS  # rule (2)
                if not ok:
                    failures.append(f"{workload}/{pass_}")
                print(f"| {workload} | {pass_} | {b:.4f} | {a:.4f} | {diff:+.4f} | {pct * 100:+.1f}% | {'yes' if ok else '**no**'} |")
            verdict = "PASS" if not failures else "FAIL (" + ", ".join(failures) + ")"
            verdicts.append((backend, gpu, res, mode, verdict))
            print(f"\n**{mode}: {verdict}**")

    # Run-to-run spread of the baseline, as a noise estimate.
    print("\n### Baseline run-to-run spread (max - min of repeated medians)\n")
    print("| Backend | GPU | Res | Workload | Pass | Spread ms |")
    print("|---|---|---|---|---|---:|")
    for (backend, gpu, mode, res, workload, pass_), values in sorted(raw.items()):
        if mode.startswith("baseline") and len(values) > 1 and pass_ == "frame":
            print(f"| {backend} | {gpu} | {res} | {workload} | {pass_} | {max(values) - min(values):.4f} |")

    print("\n## Verdicts (P79)\n")
    for backend, gpu, res, mode, verdict in verdicts:
        print(f"- {backend}, {gpu}, {res}p, `{mode}`: {verdict}")


if __name__ == "__main__":
    main()
