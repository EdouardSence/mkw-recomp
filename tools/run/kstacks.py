"""Where each thread of a run sleeps in the kernel: the kernel stacks of a `perf record -e cs -g`, named and counted.

Why: the runtime's lock profile (MKW_LOCK_PROF) does not see the kernel's own locks. The shadow sync's PAGEMAP_SCAN
and the read traps' mprotect queue on the process's mmap_lock (cs-off/cs-on, 06/11/2026), which only the context
switches' kernel stacks show. With kptr_restrict=2 the kernel hides its addresses even from root and
`perf report --kallsyms` named nothing, so the stacks are named here from a copy of /proc/kallsyms taken with
kptr_restrict lowered for that read only (same boot as the recording).

Usage:
  sudo -kA sh -c 'k=$(cat /proc/sys/kernel/kptr_restrict); echo 0 > /proc/sys/kernel/kptr_restrict;
                  cat /proc/kallsyms; echo $k > /proc/sys/kernel/kptr_restrict' > scratchpad/kallsyms
  perf record -e cs -g -p "$(pgrep -x mkw)" -o scratchpad/cs.data -- sleep 20   (once the race runs)
  uv run tools/run/kstacks.py scratchpad/cs.data scratchpad/kallsyms ['vk replay' ...] [--depth 7] [--top 12]
Without thread names: the sleeps on the kernel's rwsems (mmap_lock) per thread and syscall.
"""

import argparse
import bisect
import subprocess
from collections import Counter

# Frames of the switch itself, the same in every stack.
SCHED = {
    "__schedule",
    "schedule",
    "schedule_preempt_disabled",
    "__traceiter_sched_switch",
    "perf_trace_sched_switch",
    "__perf_event_task_sched_out",
    "perf_event_task_sched_out",
    "perf_tp_event",
    "__perf_sw_event",
    "___perf_sw_event",
}
RWSEM = {"rwsem_down_read_slowpath": "read", "rwsem_down_write_slowpath": "write", "down_write_killable": "write"}


def kernel_names(path: str):
    syms = []
    with open(path) as f:
        for line in f:
            addr, kind, name = line.split()[:3]
            if kind.lower() in "tw":
                syms.append((int(addr, 16), name))
    if not syms or not syms[-1][0]:
        raise SystemExit(f"{path}: every address is 0: copy it with kptr_restrict lowered (see the header)")
    syms.sort()
    addrs = [a for a, _ in syms]
    return lambda x: syms[bisect.bisect_right(addrs, x) - 1][1]


def samples(data: str):
    """(thread name, kernel frames innermost first) per context switch."""
    out = subprocess.run(["perf", "script", "-i", data, "-F", "comm,ip"], capture_output=True, text=True, check=True)
    comm, frames = None, []
    for line in out.stdout.splitlines():
        if line.strip() and not line[0].isspace():
            if comm is not None:
                yield comm, frames
            comm, frames = line.strip(), []
        elif line.strip():
            x = int(line.split()[0], 16)
            if x >= 0xFFFF800000000000:
                frames.append(x)
    if comm is not None:
        yield comm, frames


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("data")
    ap.add_argument("kallsyms")
    ap.add_argument("threads", nargs="*")
    ap.add_argument("--depth", type=int, default=7)
    ap.add_argument("--top", type=int, default=12)
    args = ap.parse_args()
    name = kernel_names(args.kallsyms)
    stacks: dict[str, Counter] = {t: Counter() for t in args.threads}
    rwsem: Counter = Counter()
    for comm, frames in samples(args.data):
        named = [name(x) for x in frames]
        if comm in stacks:
            stacks[comm][" < ".join([f for f in named if f not in SCHED][: args.depth])] += 1
        lock = next((RWSEM[f] for f in named if f in RWSEM), None)
        if lock:
            call = next((f for f in named if f.startswith("__x64_sys_") or f == "exc_page_fault"), "?")
            rwsem[(comm, lock, call)] += 1
    for t, c in stacks.items():
        total = sum(c.values())
        print(f"== {t}: {total} switches")
        for stack, n in c.most_common(args.top):
            print(f"  {n:6} {100 * n / total:5.1f}%  {stack}")
    if not args.threads:
        print("== sleeps on a kernel rwsem (mmap_lock), per thread, side and syscall")
        for (comm, lock, call), n in rwsem.most_common(args.top):
            print(f"  {n:6}  {comm:<16} {lock:<5} {call}")


if __name__ == "__main__":
    main()
