#!/usr/bin/env bash
# Every 5 s while build/mkw runs: its RSS, anonymous and swapped memory (MiB), amdgpu GTT and VRAM in use (MiB) and the
# machine's MemAvailable. Usage: memwatch.sh OUT.tsv
set -euo pipefail
out=$1; dev=/sys/class/drm/card1/device
echo -e "t\trss\tanon\tswap\tgtt\tvram\tavail\theap_rss\tother_anon" > "$out"
until pid=$(pgrep -x mkw); do sleep 1; done
t=0
while kill -0 "$pid" 2>/dev/null; do
  r=$(awk '/^Rss:/{r=$2} /^Anonymous:/{a=$2} /^Swap:/{s=$2} END {printf "%d\t%d\t%d", r/1024, a/1024, s/1024}' /proc/$pid/smaps_rollup 2>/dev/null || echo "0	0	0")
  g=$(( $(cat $dev/mem_info_gtt_used) / 1048576 )); v=$(( $(cat $dev/mem_info_vram_used) / 1048576 ))
  m=$(awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo)
  # resident pages inside the game's heap reservation (mem: heap ... at 0x130000000, 8 GiB) vs the rest
  h=$(awk 'BEGIN {lo = strtonum("0x130000000"); hi = lo + 8 * 2^30}
      /^[0-9a-f]+-[0-9a-f]+ / {split($1, a, "-"); s = strtonum("0x" a[1]); inh = s >= lo && s < hi}
      /^Rss:/ {if (inh) hr += $2}  /^Anonymous:/ {if (!inh) oa += $2}
      END {printf "%d\t%d", hr / 1024, oa / 1024}' /proc/$pid/smaps 2>/dev/null || echo "0	0")
  echo -e "$t\t$r\t$g\t$v\t$m\t$h" >> "$out"
  sleep 5; t=$((t+5))
done
