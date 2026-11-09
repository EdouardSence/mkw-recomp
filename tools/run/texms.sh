#!/usr/bin/env bash
# Milliseconds the render thread spent on texture uploads (and on ASTC within them) up to FRAME, summed from the perf
# lines (MKW_TRACE=perf) of a run: blocked share x 60 frames x mean interval; and the worst frame. Compares two runs
# of the same inputs. Usage: tools/run/texms.sh build/run/NAME.log [FRAME]
set -euo pipefail
log=$1 upto=${2:-1500}
rg -o 'frame [0-9]+: interval avg [0-9.]+ ms.*?texture [0-9]+% \(astc [0-9]+%\).*?slowest frame: texture [0-9.]+ ms' "$log" |
    awk -v upto="$upto" '{
        f = $2 + 0; if (f > upto) next
        match($0, /avg [0-9.]+/); avg = substr($0, RSTART + 4, RLENGTH - 4)
        match($0, /texture [0-9]+%/); t = substr($0, RSTART + 8, RLENGTH - 9)
        match($0, /astc [0-9]+%/); a = substr($0, RSTART + 5, RLENGTH - 6)
        match($0, /texture [0-9.]+ ms$/); w = substr($0, RSTART + 8, RLENGTH - 11)
        tex += t / 100 * 60 * avg; astc += a / 100 * 60 * avg; if (w + 0 > worst) worst = w + 0
    } END { printf "texture %.0f ms (astc %.0f ms), peor frame %.0f ms\n", tex, astc, worst }'
