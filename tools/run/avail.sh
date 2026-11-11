#!/usr/bin/env bash
# MB the system can give a run: MemAvailable plus GPUReclaim, the pages amdgpu's TTM pool keeps after a Vulkan process
# frees them (write-combined, reused instead of changing their caching again). The kernel does not count them in
# MemAvailable but its shrinker returns them under pressure: freeing 2 GB of Vulkan memory took MemAvailable down by
# 2 GB and GPUReclaim up by as much, and a 2 GB anonymous allocation took GPUReclaim back to 7 MB
# (08/11/2026). With --wait FLOOR it waits until there are FLOOR MB, up to WAIT_MEM minutes (default 0: no wait), and
# prints the MB it ended with: the run scripts take it, so a run queued behind a build or prep_watch starts by itself
# (11/11/2026: three runs refused below 8500 MB and three retry loops written by hand in one session).
# Usage: tools/run/avail.sh                (prints the MB)
#        WAIT_MEM=30 tools/run/avail.sh --wait 8500
set -euo pipefail
mb() { awk '/^(MemAvailable|GPUReclaim):/ {kb += $2} END {print int(kb / 1024)}' /proc/meminfo; }
if [ "${1:-}" = --wait ]; then
  end=$(( $(date +%s) + ${WAIT_MEM:-0} * 60 )); said=0
  while [ "$(mb)" -lt "$2" ] && [ "$(date +%s)" -lt "$end" ]; do
    [ "$said" = 1 ] || { echo "avail: $(mb) MB, waiting up to ${WAIT_MEM} min for $2 (WAIT_MEM)" >&2; said=1; }
    sleep 10
  done
fi
mb
