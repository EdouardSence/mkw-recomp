#!/usr/bin/env bash
# Refuses a run on the real GPU when amdgpu is in a state where the next fault can take the whole desktop with it, and
# otherwise lets it go with the pre-submit checks on (MKW_GPU_GUARD, runtime/gfx/vk/device/vk_guard.c).
# Why the line is drawn here: on 08/11/2026 the boot that froze (14:16, kwin "Pageflip timed
# out", forced power-off) had a ring reset that FAILED and a full GPU reset, then more faults; the capture replay after
# that froze the laptop. The same day, after a ring timeout whose ring reset succeeded ("device wedged, but no recovery
# needed", gpuav1 14:49), the desktop went on unharmed, and so did four more recovered at the ring that afternoon
# (xb-*, 15:18-15:20). Adrià (08/11): no waiting on him to reboot. So only a failed ring reset or a full GPU reset
# stops runs; any number of episodes recovered at the ring does not (counting them made a reboot unavoidable).
# Usage: tools/run/gpu_ok.sh   (exit 0 when the GPU may be used; MKW_GPU_TAINTED_OK=1 overrides, after reading why)
set -euo pipefail
[ "${MKW_GPU_TAINTED_OK:-0}" = 1 ] && exit 0
log=$(journalctl -k -b 0 --no-pager -o short-unix 2>/dev/null | grep amdgpu || true)
faults=$(echo "$log" | grep -E 'gfxhub\] page fault|ring .* timeout' || true)
[ -z "$faults" ] && exit 0
# Episodes: fault lines more than 60 s apart.
episodes=$(echo "$faults" | awk '{t=int($1); if (NR==1 || t-last>60) n++; last=t} END {print n+0}')
bad=$(echo "$log" | grep -E 'reset failed|GPU reset begin|VRAM is lost' || true)
if [ -n "$bad" ]; then
  echo "gpu_ok: amdgpu since boot: $episodes fault episode(s), and a failed or full GPU reset:" >&2
  echo "$bad" | head -3 | cut -c1-200 | sed 's/^/  /' >&2
  echo "gpu_ok: the next fault may freeze the desktop: no runs on the GPU" >&2
  exit 1
fi
echo "gpu_ok: $episodes fault episode(s) since boot, all recovered at the ring: going on (MKW_GPU_GUARD on in the run scripts)" >&2
exit 0
