#!/usr/bin/env bash
# Run a heavy command (a game run, a replay) through the host's job runner when there is one, else directly.
# Why: our machine runs them under ~/.agents/scripts/job (memory cap, one at a time, systemd unit); a fresh clone has
# no such runner and the scripts must still work. JOB_RUNNER=path picks another runner with the same interface.
# Usage: tools/run/job.sh run NAME CMD [ARGS...]     foreground, the command's exit status
#        tools/run/job.sh start NAME CMD [ARGS...]   background (the command redirects its own output)
set -euo pipefail

mode=$1 name=$2; shift 2
runner=${JOB_RUNNER:-$HOME/.agents/scripts/job}
if [[ -x $runner ]]; then
    exec "$runner" "$mode" "$name" "$@"
fi
case $mode in
    run) exec "$@" ;;
    start) nohup "$@" > /dev/null 2>&1 < /dev/null & ;;
    *) echo "job.sh: unknown mode $mode (run|start)" >&2; exit 2 ;;
esac
