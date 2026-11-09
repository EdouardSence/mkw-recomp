"""GPU of the race in several runs side by side: per run, the median of the 60-frame windows' «gpu avg» and frame
interval after the race start (the first window left out), the MKW_PASS_TIMING windows wholly in the race (busy = all
minus outside, and the biggest pass) and the last texture memory line.

Why: a change of the GPU's work without an in-run switch (MKW_ASTC_BC1 picks formats at image creation) is compared
between runs; the frame interval there is noisy (up to 12 ms between equal runs), the GPU's own time less so.

Usage: uv run tools/run/gpucmp.py build/run/NAME.log [...]
"""

import re
import statistics
import sys
from pathlib import Path

START = re.compile(r"seq: enter IsSequenceFlag.* frame (\d+)|frame (\d+).*seq: enter IsSequenceFlag")
GPU = re.compile(r"perf: frame (\d+): gpu avg ([\d.]+) ms")
INTERVAL = re.compile(r"perf: frame (\d+): interval avg ([\d.]+) ms")
PASSES = re.compile(
    r"GPU ms per frame by pass size and compute program at frame (\d+) \(all ([\d.]+);.*?; outside ([\d.]+)"
)
SCENE = re.compile(r"; (\d+x\d+) ([\d.]+) \([\d.]+ passes\)")
TEXTURES = re.compile(r"textures live (\d+) \(own memory \d+, (\d+) MiB\)")


def race_start(text: str) -> int:
    """The frame of the last IsSequenceFlag entry (the race; earlier ones are menus), 0 if none."""
    frames = [int(a or b) for a, b in START.findall(text)]
    return frames[-1] if frames else 0


def median_after(rx: re.Pattern[str], text: str, start: int) -> tuple[float, int]:
    vals = [float(v) for f, v in rx.findall(text) if int(f) - 60 > start + 60]  # wholly in the race, not the first
    return (statistics.median(vals), len(vals)) if vals else (float("nan"), 0)


def main() -> None:
    for name in sys.argv[1:]:
        text = Path(name).read_text(errors="replace")
        start = race_start(text)
        gpu, n = median_after(GPU, text, start)
        itv, _ = median_after(INTERVAL, text, start)
        tex = TEXTURES.findall(text)
        print(
            f"{Path(name).stem}: race from frame {start}; {n} windows: gpu {gpu:.2f} ms, interval {itv:.2f} ms; "
            f"textures {tex[-1][1] if tex else '?'} MiB"
        )
        for m in PASSES.finditer(text):
            frame = int(m.group(1))
            if frame - 600 < start:
                continue
            line = text[m.start() : text.find("\n", m.start())]
            top = max(SCENE.findall(line), key=lambda p: float(p[1]), default=("?", "0"))
            busy = float(m.group(2)) - float(m.group(3))
            print(f"  passes at frame {frame}: busy {busy:.2f} ms, {top[0]} {top[1]} ms")


if __name__ == "__main__":
    main()
