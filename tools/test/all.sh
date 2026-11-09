#!/usr/bin/env bash
# Every capture under build/cap through tools/test/replay.sh, one after another (one GPU job at a time): the regression
# test of the back end without the game. Prints each capture's replay line and its worst frame against the game.
# Usage: tools/test/all.sh [NAME]   (NAME of this test run, default "all"; MODES as replay.sh)
set -euo pipefail
cd "$(dirname "$0")/../.."
name=${1:-all} status=0
for cap in build/cap/*/; do
  cap=${cap%/}
  [ -f "$cap/capture.zst" ] || continue
  echo "### ${cap##*/}"
  tools/test/replay.sh "$cap" "$name" | grep -E "^replay:|FAILED|^== |median|worst" || status=1
done
exit $status
