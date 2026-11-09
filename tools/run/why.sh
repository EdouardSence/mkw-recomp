# shellcheck shell=bash
# Sourced by the scripts that start a game run (run_perf.sh, live.sh): a run started by an agent must say
# what it expects to see and what each result means, in MKW_RUN_WHY. Rules alone did not hold: on 07/11/2026 the
# session started 22 min after "test the replay without the game first" still launched 59 runs, against 81 replays.
# Only under Claude Code (CLAUDECODE=1); by hand nothing is asked. Each reason goes to build/run/why.log.
# Usage: . tools/run/why.sh NAME
if [ -n "${CLAUDECODE:-}" ] && [ -z "${MKW_RUN_WHY:-}" ]; then
  caps=$(ls "$(dirname "${BASH_SOURCE[0]}")/../../build/cap" 2>/dev/null | tr '\n' ' ')
  echo "${0##*/}: a run needs MKW_RUN_WHY=\"what I expect to see; what I conclude from each result\"." >&2
  echo "A back-end change (replay, translator, textures, memory) is checked on a capture in seconds:" \
    "tools/test/replay.sh build/cap/<name> (captures here: ${caps:-none}; a new one with MKW_CAPTURE)." >&2
  exit 1
fi
if [ -n "${MKW_RUN_WHY:-}" ]; then
  why_log=$(dirname "${BASH_SOURCE[0]}")/../../build/run/why.log; mkdir -p "${why_log%/*}"
  printf '%s\t%s\t%s\n' "$(date '+%F %T')" "${MKW_RUN_NAME:-${1:-?}}" "$MKW_RUN_WHY" >> "$why_log"
fi
