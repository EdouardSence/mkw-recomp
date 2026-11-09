#!/usr/bin/env bash
# Runs ctl commands on a live run when the game enters one of its own phases (sequence nodes, MKW_SEQ_WATCH=1 in the
# run: runtime/core/diag/seqwatch.c), instead of at a frame number that moves with every change of pacing.
# Usage: tools/run/at.sh [--past] NAME [PREV,]NODE [+FRAMES[,FRAMES...]] COMMAND [ARG...] [-- COMMAND [ARG...]]...
#   tools/run/at.sh live30 WaitIntroMovie +150 shot scratchpad/tmp/video.png -- dump scratchpad/dump-video
#   tools/run/at.sh vis8 -WaitIntroMovie,IsSequenceFlag +140,160,200 shot scratchpad/tmp/vis8/r{k}.png
# Waits for the next "seq: enter NODE" line of build/run/NAME.log after now (or one already there with --past), then
# FRAMES presents more, then each command through tools/run/ctl.sh. PREV,NODE: the first NODE after PREV entered
# (-PREV: after PREV left), as MKW_SEQ_UNTIL: IsSequenceFlag also enters in the menus, the race's comes after the cup
# movie. A list of FRAMES runs the commands at each, {k} in an argument becoming that offset (one live run, a capture
# every few frames: the 01/11/2026 sessions wrote that loop by hand eight times). Node names: build/seq_nodes.tsv
# (third column), the order of a run: grep 'seq: enter' build/run/NAME.log.
set -euo pipefail
cd "$(dirname "$0")/../.."
past=0
[ "${1:-}" = --past ] && { past=1; shift; }
n=${1:?usage: at.sh [--past] NAME [PREV,]NODE [+FRAMES[,...]] COMMAND...}; node=${2:?node}; shift 2
prev=""
if [[ $node == *,* ]]; then prev=${node%%,*}; node=${node#*,}; fi
# "-NODE" alone waits for a node named "-NODE", which never comes (06/11/2026: two runs lost to "-WaitIntroMovie")
[[ $node == -* ]] && { echo "at.sh: $node: a leave is PREV,NODE (-WaitIntroMovie,IsSequenceFlag), not a node" >&2; exit 2; }
offsets=(0)
if [[ ${1:-} == +* ]]; then IFS=, read -r -a offsets <<< "${1#+}"; shift; fi
[ $# -gt 0 ] || { echo "at.sh: no command" >&2; exit 2; }
log=build/run/$n.log
for _ in $(seq 120); do [ -f "$log" ] && break; sleep 0.5; done   # live.sh right before: the log comes a moment later
[ -f "$log" ] || { echo "at.sh: no $log" >&2; exit 1; }
start=$((past ? 0 : $(wc -l < "$log")))
frame() { tools/run/ctl.sh "$n" frame 2>/dev/null | grep -o '[0-9]\+$' || echo 0; }
alive() { pgrep -x mkw >/dev/null; }
# Line number of the first "seq: <what> NODE frame" after line $1; nothing when there is none yet.
line_of() { awk -v s="$1" -v pat="seq: $2 $3 frame" 'NR > s && index($0, pat) {print NR; exit}' "$log"; }
wait_line() {   # waits for "seq: $2 $3" after line $1 and prints its line number
    local l
    until l=$(line_of "$1" "$2" "$3") && [ -n "$l" ]; do
        alive || { echo "at.sh: run ended before $2 $3" >&2; exit 1; }
        sleep 0.5
    done
    echo "$l"
}
if [ -n "$prev" ]; then
    if [[ $prev == -* ]]; then start=$(wait_line "$start" leave "${prev#-}"); else start=$(wait_line "$start" enter "$prev"); fi
fi
l=$(wait_line "$start" enter "$node")
[ -p "build/run/$n.ctl" ] || { echo "at.sh: $n has no control FIFO (build/run/$n.ctl): started without MKW_CTL" >&2; exit 1; }
f0=$(sed -n "${l}p" "$log" | grep -o 'frame [0-9]*' | grep -o '[0-9]*')
echo "at.sh: $node entered at frame $f0"
for k in "${offsets[@]}"; do
    until [ "$(frame)" -ge $((f0 + k)) ]; do alive || exit 1; sleep 0.03; done
    cmd=()
    for a in "$@" --; do
        if [ "$a" != -- ]; then cmd+=("${a//\{k\}/$k}"); continue; fi
        if [ ${#cmd[@]} -gt 0 ]; then tools/run/ctl.sh "$n" "${cmd[@]}"; fi
        cmd=()
    done
done
