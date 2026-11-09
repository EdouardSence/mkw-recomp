#!/usr/bin/env bash
# MB the system can give a run: MemAvailable plus GPUReclaim, the pages amdgpu's TTM pool keeps after a Vulkan process
# frees them (write-combined, reused instead of changing their caching again). The kernel does not count them in
# MemAvailable but its shrinker returns them under pressure: freeing 2 GB of Vulkan memory took MemAvailable down by
# 2 GB and GPUReclaim up by as much, and a 2 GB anonymous allocation took GPUReclaim back to 7 MB
# (08/11/2026). Usage: tools/run/avail.sh   (prints the MB)
set -euo pipefail
awk '/^(MemAvailable|GPUReclaim):/ {kb += $2} END {print int(kb / 1024)}' /proc/meminfo
