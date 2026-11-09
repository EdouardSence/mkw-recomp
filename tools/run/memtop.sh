#!/usr/bin/env bash
# At AT seconds after build/mkw starts: every mapping with its Rss (MiB), address range and name.
# Usage: memtop.sh AT OUT
set -euo pipefail
at=$1; out=$2
until pid=$(pgrep -x mkw); do sleep 1; done
sleep "$at"
awk "/^[0-9a-f]+-[0-9a-f]+ /{r=\$1; n=\$6} /^Pss:/{print int(\$2/1024), r, n}" /proc/$pid/smaps > "$out"
