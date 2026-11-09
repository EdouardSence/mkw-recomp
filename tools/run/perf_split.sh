#!/usr/bin/env bash
# Split a perf profile of build/mkw by owner: guest code (f_*), runtime, driver libraries, others; then the top 15
# guest functions with their module offset (q.py-ready). Usage: perf_split.sh FILE.perf
set -euo pipefail
perf report -i "$1" --stdio --no-children --sort dso,symbol 2>/dev/null | grep -E '^ +[0-9.]+%' > "${1%.perf}.sym.txt"
awk '{p=$1; sub("%","",p); d=$2; s=$NF;
      k = (s ~ /^f_7/) ? "guest" : (d ~ /mkw/) ? "runtime" : (d ~ /radeon|vulkan|amdgpu|drm/) ? "driver" : "other:" d;
      t[k]+=p} END {for (k in t) printf "%6.2f%% %s\n", t[k], k}' "${1%.perf}.sym.txt" | sort -rn | head -12
echo "-- top guest functions"
grep ' f_7' "${1%.perf}.sym.txt" | head -15 | awk '{print $1, $NF}'
