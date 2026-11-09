"""One-screen summary of a game run's log: how it ended, how far it got, frame times, memory and failures.

Why: every run was read with the same dozen greps (frame lines, FATAL, first calls, mem: vram, image creation
failed...), 767 of them over the sessions to 01/11/2026. This prints those answers in ~15 lines.

Usage: uv run tools/run/summary.py build/run/NAME.log [...]
Host addresses of a FATAL: tools/run/sym.sh < build/run/NAME.log.
"""

import re
import statistics
import sys
from collections import Counter
from pathlib import Path

PERF = re.compile(r"perf: frame (\d+): interval avg ([\d.]+) ms max ([\d.]+) ms.*?(?:world (-?[\d.]+) s)?$")
FRAME = re.compile(r"(?:perf|present|vk_exec|seq: \w+ \w+)\D*frame (\d+)")
VRAM = re.compile(r"mem: vram frame (\d+): vma \d+ blocks (\d+) MiB.*heap1 (\d+)/(\d+) MiB")
SEQ = re.compile(r"seq: enter (\w+) frame (\d+)")
RACE = re.compile(r"seq: watch off at the start of (\w+) frame (\d+)")
# "has another format ... rewritten": a file of ours that every run throws away (pipeline-keys.bin from 05/11/2026 to
# 06/11/2026: the boot prewarm read nothing for a day and no summary showed it)
SPEED = re.compile(r"game: speed ([\d.]+)x over ([\d.]+) s \((\d+) frames, (\d+) interval changes")
SLOW = 0.98  # game seconds per real second below which the game runs in slow motion (runtime/core/boot/game.c)
FAIL = re.compile(r"\[host\] ((?:[\w ]+: )+)(.*?(?:[a-z] failed|not implemented|cannot \w+|over MKW_\w+|device lost|another format.*rewritten))")


def pct(xs: list[float], q: float) -> float:
    return sorted(xs)[min(len(xs) - 1, int(q * len(xs)))]


def frames(label: str, xs: list[tuple[float, float]]) -> str:
    if not xs:
        return f"{label}: no perf lines (MKW_TRACE=perf or MKW_FLIGHT prints them every 60 frames; runtime/core/diag/perf.c perf_base)"
    avg = [a for a, _ in xs]
    return (
        f"{label}: {len(xs)} windows of 60 frames, interval median {statistics.median(avg):.1f} ms, "
        f"p95 {pct(avg, 0.95):.1f}, worst max {max(m for _, m in xs):.1f}"
    )


def host_time(lines: list[str]) -> list[str]:
    """The last MKW_HOST_TIME census, its five costliest functions."""
    starts = [i for i, ln in enumerate(lines) if "host time: frame" in ln]
    if not starts:
        return []
    head = lines[starts[-1]].split("host time: ", 1)[1].split(";")[0]
    rows = []
    for ln in lines[starts[-1] + 1 :]:
        m = re.search(r"([\d.]+) ms/s render\s+([\d.]+) ms/s other\s+(\d+) calls\s+(\S+)", ln)
        if not m:
            break
        rows.append((float(m[1]) + float(m[2]), f"{m[4]} {m[1]}+{m[2]}"))
    top = ", ".join(r for _, r in sorted(rows, reverse=True)[:5])
    return [f"host time ({head}, ms/s render+other): {top}"]


def speed(lines: list[str]) -> list[str]:
    """Game speed before and in the race, from game.c's 5 s windows; a warning when the race runs slow (09/11/2026:
    the present interval flipped 1<->2 at ~30 fps and the race ran in slow motion, in every log and unread for days)."""
    race_at = next((i for i, ln in enumerate(lines) if RACE.search(ln)), len(lines))
    parts: dict[str, list[tuple[float, float, int]]] = {"before the race": [], "race": []}
    for i, ln in enumerate(lines):
        if m := SPEED.search(ln):
            parts["race" if i > race_at else "before the race"].append((float(m[1]), float(m[2]), int(m[4])))
    out = []
    for label, xs in parts.items():
        if not xs:
            continue
        secs = sum(s for _, s, _ in xs)
        mean = sum(v * s for v, s, _ in xs) / secs
        flips = sum(c for _, _, c in xs) / secs * 60
        warn = f"  WARNING: slow motion (below {SLOW}x)" if label == "race" and mean < SLOW else ""
        out.append(f"game speed {label}: {mean:.2f}x over {secs:.0f} s, {flips:.0f} interval changes/min{warn}")
    return out


def summary(path: Path) -> list[str]:
    lines = path.read_text(errors="replace").splitlines()
    text = "\n".join(lines)
    out = [f"== {path}"]

    fatal = [ln for ln in lines if "FATAL: " in ln or "fatal signal" in ln or "in lifted function" in ln]
    exits = [ln for ln in lines if re.search(r"MKW_SEQ_EXIT: .*exit|window closed; exiting|frametime: exit", ln)]
    if fatal:
        out.append("end: " + " | ".join(ln.strip()[:200] for ln in fatal[:3]))
    elif exits:
        out.append("end: " + exits[0].split("] ", 1)[-1][:160])
    else:
        out.append("end: no exit line (killed: timeout, ctl stop, OOM; or still running)")

    last = max((int(m[1]) for m in FRAME.finditer(text)), default=0)
    seq = [(m[1], m[2]) for m in SEQ.finditer(text)]
    phases = [n for i, (n, _) in enumerate(seq) if i == 0 or seq[i - 1][0] != n]
    out.append(f"last frame {last}" + (f"; phases ({len(phases)}): ... {' > '.join(phases[-6:])}" if phases else ""))

    race = RACE.search(text)
    if race:
        out.append(f"race start: {race[1]} at frame {race[2]}")
    pre, inrace = [], []
    for ln in lines:
        m = PERF.search(ln)
        if m:  # in the race: the world clock runs, or (logs where it stays 0) past the race's first frame
            racing = (m[4] is not None and float(m[4]) > 0) or (race is not None and int(m[1]) > int(race[2]))
            (inrace if racing else pre).append((float(m[2]), float(m[3])))
    out.append(frames("before the race" if inrace else "frames", pre))
    if inrace:
        out.append(frames("race", inrace))
    out += [ln.split("] ", 1)[-1][:160] for ln in lines if "frametime: exit" in ln]
    out += speed(lines)

    vram = [(int(m[1]), int(m[2]), int(m[3]), int(m[4])) for m in VRAM.finditer(text)]
    if vram:
        f, vma, used, budget = max(vram, key=lambda v: v[2])
        out.append(f"vram peak: heap1 {used}/{budget} MiB at frame {f} (vma {vma} MiB)")
    out += host_time(lines)

    fails = Counter(re.sub(r"0x[0-9a-f]+|\d+", "N", m[1] + m[2])[-90:] for ln in lines if (m := FAIL.search(ln)))
    if fails:
        out.append("failures: " + "; ".join(f"{n}x {k}" for k, n in fails.most_common(6)))
    first = sum("first call" in ln for ln in lines)
    if first:
        out.append(f"nvn first calls: {first}")
    return out


def main() -> None:
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for p in sys.argv[1:]:
        print("\n".join(summary(Path(p))))


if __name__ == "__main__":
    main()
