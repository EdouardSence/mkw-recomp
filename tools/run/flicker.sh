#!/usr/bin/env bash
# Counts the frames whose lower half is black in a flight dump taken from a live run (tools/run/live.sh): the title's
# 3D scene flickering to black with the logo kept (31/10/2026, about 1 frame in 10). A dump covers the 600 frames up to
# the request. Usage: tools/run/flicker.sh NAME [AT_FRAME] (default 900: the title, before any press)
set -euo pipefail
cd "$(dirname "$0")/../.."
n=${1:?usage: flicker.sh NAME [AT_FRAME]}; at=${2:-900}
until [ "$(tools/run/ctl.sh "$n" frame 2>/dev/null | grep -o '[0-9]*$' || echo 0)" -ge "$at" ]; do sleep 2; done
tools/run/ctl.sh "$n" flight | tail -1
sleep 8   # the dump holds 30 frames after the request and is written by a background thread
d=$(ls -dt scratchpad/flight-"$n"/*/ | head -1)
uv run -q --with pillow python3 - "$d" <<'PY'
import sys
from pathlib import Path
from PIL import Image
ps = sorted(Path(sys.argv[1]).glob("f*.png"))
dark = [p.stem for p in ps if sum(Image.open(p).convert("L").resize((32, 18)).crop((0, 9, 32, 18)).get_flattened_data()) / 288 < 15]
print(f"{len(ps)} frames, {len(dark)} with a black lower half", dark[:10])
PY
