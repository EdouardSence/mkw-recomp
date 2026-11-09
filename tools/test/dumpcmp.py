# /// script
# dependencies = []
# ///
"""Two MKW_DUMP folders of the same frame (the game's, saved with a capture, and a replay's) compared pass by pass.

Why: a replay that differs from the game is found at the first pass whose targets differ, not by looking at the final
image (the race replay was dark with every input apparently there, 07/11/2026). Each dump's images.txt holds, per
image, its min, max and distinct values. Consecutive passes writing the same targets are one group (the game's
asynchronous replay splits a pass every 64 draws, a replay without the game does not), and groups are matched in order,
compared by the state of their targets after the group's last pass.

Usage: uv run tools/test/dumpcmp.py GAME_DUMP REPLAY_DUMP [--all]
  Prints every pass target (t0..t7, td) whose statistics differ, first ones first; --all also the textures read.
"""

import argparse
import re
from pathlib import Path

LINE = re.compile(r"^(pass_(\d+)_(\w+?)_\S+) (\S+) min (.*?) max (.*?) distinct (\d+)")


TARGET = re.compile(r"^  (t[0-7d]): .* addr (0x[0-9a-f]+)(?: image \S+)? -> (\S+)\.png")
PASS = re.compile(r"^pass (\d+): (render pass|dispatch|copy|clear)")


def groups(d: Path) -> list[tuple[str, list[tuple[str, str]]]]:
    """(key, [(tag, image name)]) per group of consecutive passes with the same targets, the last pass's names."""
    out, cur = [], []
    for line in (d / "passes.txt").read_text().splitlines():
        if PASS.match(line):
            if cur:
                k = " ".join(f"{t}@{a}" for t, a, _ in cur)
                if out and out[-1][0] == k:
                    out[-1] = (k, [(t, n) for t, _, n in cur])
                else:
                    out.append((k, [(t, n) for t, _, n in cur]))
            cur = []
            continue
        m = TARGET.match(line)
        if m:
            cur.append((m[1], m[2], m[3]))
    if cur:
        out.append((" ".join(f"{t}@{a}" for t, a, _ in cur), [(t, n) for t, _, n in cur]))
    return out


def load(d: Path, every: bool) -> dict[tuple[int, str], tuple[str, str, str, int]]:
    out = {}
    for line in (d / "images.txt").read_text().splitlines():
        m = LINE.match(line)
        if not m:
            continue
        name, npass, what = m[1], int(m[2]), m[3]
        if not every and not re.fullmatch(r"t[0-7d]", what):
            continue
        out[(npass, what)] = (name, m[5], m[6], int(m[7]))
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("game", type=Path)
    ap.add_argument("replay", type=Path)
    ap.add_argument("--all", action="store_true")
    a = ap.parse_args()
    stats = {}
    for d in (a.game, a.replay):
        for line in (d / "images.txt").read_text().splitlines():
            m = LINE.match(line)
            if m:
                stats[(d, m[1])] = (m[5], m[6], int(m[7]))
    gg, rg = groups(a.game), groups(a.replay)
    shown = 0
    for i, ((gk, gn), (rk, rn)) in enumerate(zip(gg, rg)):
        if gk.split("@")[0] != rk.split("@")[0] or len(gn) != len(rn):
            print(f"group {i}: different targets (game {gk[:80]} / replay {rk[:80]}): passes no longer line up")
            break
        for (t, x), (_, y) in zip(gn, rn):
            sx, sy = stats.get((a.game, x)), stats.get((a.replay, y))
            if sx and sy and sx[:2] == sy[:2] and abs(sx[2] - sy[2]) <= max(8, sx[2] // 50):
                continue
            print(f"group {i:3d} {t:3s} game {x}: {sx}\n{'':14s}replay {y}: {sy}")
            shown += 1
        if shown >= 40:
            break
    print(f"{len(gg)} game groups, {len(rg)} replay groups, {shown} targets differ (first 40)")


if __name__ == "__main__":
    main()
