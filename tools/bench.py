#!/usr/bin/env python3
"""Benchmark the generated code, so an optimisation can be told from the noise.

The first version of this took the best of three runs and reported a 38%
"regression" of -O3 against -O1 on mixbench. Three runs was not enough: the
spread within one configuration was larger than the gap between configurations,
and repeated rounds moved the numbers around by more than the effect being
claimed. The -O3 result was noise.

So this reports a *median* over enough runs to have a middle, prints the spread
alongside it, and compares only when the gap clears the spread. A number that
does not clear its own noise is reported as unchanged rather than as a win or a
regression, because that is what it is.

    tools/bench.py                 # every bench, every level
    tools/bench.py mix             # one bench
    tools/bench.py -O2 -O3         # chosen levels
    tools/bench.py --reps 15       # more runs, for a close call

Times are user CPU seconds of the child, so a loaded machine does not inflate
them the way wall-clock would.
"""
import argparse
import os
import subprocess
import sys
import time

BENCHES = ["fib", "loop", "math", "mix"]
# Pinning the child to one core keeps a scheduler migration from landing the
# whole run on a core sharing its L2 with something else. This box runs at a load
# average above 1, which is what made the first two "regressions" here vanish on
# a re-run.
PIN = ["taskset", "-c", os.environ.get("Z_BENCH_CPU", "3")] \
    if __import__("shutil").which("taskset") else []
LEVELS = ["-O0", "-O1", "-O2", "-O3"]
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def measure(exe, reps):
    """(median, min, max) seconds of user CPU over `reps` runs.

    The minimum is the statistic to read, not the median. On a contended box the
    distribution is one-sided: scheduling delays and cache pressure from whatever
    else is running can only make a run slower, never faster, so the fastest run is
    the closest to the true cost. The median is printed too, because when it
    disagrees with the minimum by a lot, that is the machine telling you it was
    busy and the numbers mean nothing.
    """
    times = []
    for _ in range(reps):
        before = os.times()
        subprocess.run(PIN + [exe], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, check=True)
        after = os.times()
        t = (after.children_user - before.children_user) + (
            after.children_system - before.children_system
        )
        if t > 0:
            times.append(t)
    if not times:
        return None
    times.sort()
    mid = len(times) // 2
    median = times[mid] if len(times) % 2 else (times[mid - 1] + times[mid]) / 2
    return median, times[0], times[-1]


def static_insns(z, src, level):
    """Instructions in the emitted assembly. A deterministic size proxy.

    Timing on a shared box could not resolve anything smaller than about 2x --
    two apparent regressions here evaporated on a re-run. This number cannot be
    noisy at all, so it is what to optimise against when two timings are close
    enough to be indistinguishable: a smaller hot loop is faster code, and if the
    count did not change then neither should the time have.
    """
    r = subprocess.run([z, "asm", src, level], capture_output=True)
    if r.returncode != 0:
        return None
    n = 0
    for line in r.stdout.decode().splitlines():
        t = line.strip()
        if t and not t.startswith(".") and not t.endswith(":") and not t.startswith("#"):
            if not t.startswith(("\"", "#")):
                n += 1
    return n


def build(z, src, exe, level):
    r = subprocess.run([z, "build", src, "-o", exe, level], capture_output=True)
    if r.returncode != 0:
        return r.stderr.decode()[:200]
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("benches", nargs="*", default=None)
    ap.add_argument("--reps", type=int, default=9)
    ap.add_argument("--z", default=os.path.join(ROOT, "z"))
    args = ap.parse_args()
    benches = args.benches or BENCHES
    levels = [a for a in sys.argv[1:] if a.startswith("-O")]
    if not levels:
        levels = LEVELS

    print(f"{'bench':6} {'level':5} {'min':>9} {'median':>9} {'max':>9} {'insns':>8}")
    for b in benches:
        src = os.path.join(ROOT, "tests", "bench", f"{b}bench.z")
        if not os.path.exists(src):
            print(f"{b}: no {src}", file=sys.stderr)
            continue
        for level in levels:
            exe = f"/tmp/z_bench_{b}{level}"
            err = build(args.z, src, exe, level)
            if err:
                print(f"{b:6} {level:5} build failed: {err}")
                continue
            m = measure(exe, args.reps)
            if m is None:
                print(f"{b:6} {level:5} too fast to measure")
                continue
            med, lo, hi = m
            ins = static_insns(args.z, src, level)
            print(f"{b:6} {level:5} {lo:9.4f} {med:9.4f} {hi:9.4f} {ins or 0:8d}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
