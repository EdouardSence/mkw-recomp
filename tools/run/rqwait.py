"""CPU time and run-queue wait of each thread of a running build/mkw over a window, grouped by thread name.

Why: a change that lets more guest threads run at once (MKW_CORE_NUM_GATE) can
take host CPU from the replay and render threads; /proc/<pid>/task/<tid>/schedstat gives, per thread, the time on a
CPU and the time ready but waiting for one (the run queue), which no log of the runtime measures.

Waits for the run's log to show the race start (MKW_SEQ_UNTIL's "watch off" line), skips --skip seconds, then reads
schedstat at the start and the end of --secs seconds. Threads alive at both ends count. Per name (the guest threads
share theirs): threads, CPU ms per s and run-queue wait ms per s, and the share of wait in (CPU + wait).

Usage: uv run tools/run/rqwait.py build/run/NAME.log [--skip 6] [--secs 12] [--top 20]   (while the run goes)
"""

import argparse
import subprocess
import time
from collections import defaultdict
from pathlib import Path

RACE = "seq: watch off"


def mkw_pid() -> int:
    out = subprocess.run(["pgrep", "-x", "mkw"], check=False, capture_output=True, text=True).stdout.split()
    if len(out) != 1:
        raise SystemExit(f"want one mkw process, found {len(out)}")
    return int(out[0])


def sample(pid: int) -> dict[int, tuple[str, int, int]]:
    rows = {}
    for task in Path(f"/proc/{pid}/task").iterdir():
        try:
            name = (task / "comm").read_text().strip()
            run_ns, wait_ns, _ = (int(x) for x in (task / "schedstat").read_text().split())
        except FileNotFoundError, ProcessLookupError, ValueError:
            continue  # the thread ended between listing and reading
        rows[int(task.name)] = (name, run_ns, wait_ns)
    return rows


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("log", type=Path)
    ap.add_argument("--skip", type=float, default=6)
    ap.add_argument("--secs", type=float, default=12)
    ap.add_argument("--top", type=int, default=20)
    a = ap.parse_args()
    while not (a.log.exists() and RACE in a.log.read_text(errors="replace")):
        time.sleep(1)
    pid = mkw_pid()
    time.sleep(a.skip)
    t0, s0 = time.monotonic(), sample(pid)
    time.sleep(a.secs)
    t1, s1 = time.monotonic(), sample(pid)
    span = t1 - t0
    agg: dict[str, list[float]] = defaultdict(lambda: [0, 0.0, 0.0])
    for tid, (name, run1, wait1) in s1.items():
        if tid not in s0:
            continue
        _, run0, wait0 = s0[tid]
        g = agg[name]
        g[0] += 1
        g[1] += (run1 - run0) / 1e6 / span
        g[2] += (wait1 - wait0) / 1e6 / span
    total_run = sum(g[1] for g in agg.values())
    total_wait = sum(g[2] for g in agg.values())
    print(
        f"{a.log.stem}: {span:.1f} s from {a.skip:.0f} s after the race start; CPU {total_run:.0f} ms/s "
        f"({total_run / 1000:.2f} CPUs), run-queue wait {total_wait:.0f} ms/s"
    )
    print(f"{'name':16} {'thr':>4} {'cpu ms/s':>9} {'wait ms/s':>10} {'wait %':>7}")
    for name, (n, run, wait) in sorted(agg.items(), key=lambda kv: -(kv[1][1] + kv[1][2]))[: a.top]:
        print(f"{name:16} {n:4d} {run:9.1f} {wait:10.1f} {100 * wait / max(run + wait, 1e-9):6.1f}%")


if __name__ == "__main__":
    main()
