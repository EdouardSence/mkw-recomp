#!/usr/bin/env bash
# Poor man's profiler for build/mkw when perf cannot record call graphs (perf_event_paranoid 2; with 1, as on this
# machine since 06/11/2026, `perf record -g` works and tools/run/kstacks.py names the kernel side): N gdb batch
# backtraces of every thread, GAP seconds apart, into DIR. Needs the game started with MKW_ALLOW_PTRACE=1.
# Usage: sample_stacks.sh DIR [N] [GAP]
set -euo pipefail
dir=$1; n=${2:-12}; gap=${3:-2}
mkdir -p "$dir"
pid=$(pgrep -x mkw | head -1)
for i in $(seq 1 "$n"); do
    gdb -p "$pid" -batch -ex "set pagination off" -ex "thread apply all bt 12" > "$dir/s$i.txt" 2>/dev/null || true
    sleep "$gap"
done
