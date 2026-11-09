# /// script
# dependencies = ["numpy", "pillow"]
# ///
"""Isolated single-frame pops in flight thumbnails: something drawn or missing for one frame only (the flickering
trees, 06/11/2026; looking at it by eye missed and invented cases). Per 10x10 block, frame k is far from both
neighbours while they agree; an event is a frame with at least MIN such blocks (under 250: not a cut) whose neighbours
have under a quarter as many (a moving object pops every frame, a flicker once). Validated by hand on arb2/arb6: trees,
a cactus and a bus that vanishes.
Usage: uv run tools/imagen/pops.py DIR... [--min N] [--sheet OUT.png]  ->  dump, frame, blocks, bounding box
(thumbnail pixels); totals. --sheet: one row per event, frames k-1, k, k+1, to tell a flicker from an animation.
--ab LOG [--period S] [--invert]: pops and frames per MKW_AB block (B = the switch on), from each frame's time in the
dumps' frames.tsv: blocks of S seconds (6) in the order A B B A from the race start (the log's last
"seq: enter IsSequenceFlag"), as the world clock counts them (checked on lf9's logged edges)."""

import argparse
import csv
import re
from concurrent.futures import ProcessPoolExecutor
from functools import partial
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

B = 10


def events(d: Path, mn: int) -> tuple[int, list[tuple[int, str]]]:
    ps = sorted(d.glob("f*.png"))

    def block_means(p: Path) -> np.ndarray:
        f = np.asarray(Image.open(p).convert("RGB"), dtype=np.float32)
        h, w, _ = f.shape
        return f[: h // B * B, : w // B * B].reshape(h // B, B, w // B, B, 3).mean(axis=(1, 3))

    # only the block means are kept: the whole frames took ~415 MB per 600-frame dump
    blk = np.stack([block_means(p) for p in ps])
    n = len(ps)
    h, w = blk.shape[1] * B, blk.shape[2] * B
    pops = np.zeros((n, h // B, w // B), bool)
    for k in range(1, n - 1):
        a, b, c = blk[k - 1], blk[k], blk[k + 1]
        pops[k] = (np.abs(b - (a + c) / 2).max(-1) > 25) & (np.abs(a - c).max(-1) < 10)
    cnt = pops.sum(axis=(1, 2))
    out = []
    for k in range(2, n - 2):
        if mn <= cnt[k] < 250 and cnt[k - 1] < cnt[k] / 4 and cnt[k + 1] < cnt[k] / 4:
            ys, xs = np.nonzero(pops[k])
            box = f"x{xs.min() * B}-{xs.max() * B + B} y{ys.min() * B}-{ys.max() * B + B}"
            out.append((int(ps[k].stem[1:]), f"{ps[k].stem[1:].lstrip('0')} {int(cnt[k])} {box}"))
    return n, out


def frame_times(dumps: list[Path]) -> dict[int, float]:
    t = {}
    for d in dumps:  # a dump cut short by the run's end has no frames.tsv: its frames count in no block
        if not (d / "frames.tsv").exists():
            continue
        with open(d / "frames.tsv") as f:
            for row in csv.DictReader((line for line in f if not line.startswith("#")), delimiter="\t"):
                t[int(row["frame"])] = float(row["t_ms"])
    return t


def ab_blocks(a: argparse.Namespace, dumps: list[Path], rows: list[tuple[Path, int]]) -> None:
    with open(a.ab, errors="replace") as f:
        starts = [int(m.group(1)) for line in f if (m := re.search(r"seq: enter IsSequenceFlag frame (\d+)", line))]
    t = frame_times(dumps)
    if not starts or not t:
        print("ab: no race start in the log or no frames.tsv")
        return
    f0 = min(t)  # a start before the first dumped frame: back from it at the dump's mean interval
    t0 = t[starts[-1]] if starts[-1] in t else t[f0] - (f0 - starts[-1]) * (t[max(t)] - t[f0]) / max(1, max(t) - f0)

    def is_b(f: int) -> bool:
        if t[f] < t0:
            return False
        return (int((t[f] - t0) / (a.period * 1000)) % 4 in (1, 2)) != a.invert

    frames = {True: 0, False: 0}
    for f in t:
        if t[f] >= t0:
            frames[is_b(f)] += 1
    pops = {True: [], False: []}
    for _, f in rows:
        if f in t and t[f] >= t0:
            pops[is_b(f)].append(f)
    for k, name in ((True, "B (on)"), (False, "A (off)")):
        print(f"{name}: {len(pops[k])} pops in {frames[k]} race frames {pops[k]}")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("dirs", nargs="+", type=Path, help="flight dumps (folders of f*.png) or folders of them")
    ap.add_argument("--min", type=int, default=12)
    ap.add_argument("--sheet", type=Path)
    ap.add_argument("--ab", type=Path, help="the run's log")
    ap.add_argument("--period", type=float, default=6.0)
    ap.add_argument("--invert", action="store_true")
    a = ap.parse_args()
    dumps = [
        p for d in a.dirs for p in ([d] if any(d.glob("f*.png")) else sorted(x for x in d.iterdir() if x.is_dir()))
    ]
    frames = total = 0
    rows = []
    # one process per dump: 7 dumps of 600 frames took 15 s in one core (08/11/2026)
    with ProcessPoolExecutor() as pool:
        found = list(pool.map(partial(events, mn=a.min), dumps))
    for d, (n, ev) in zip(dumps, found):
        frames, total = frames + n, total + len(ev)
        for f, e in ev:
            print(d.name, e)
            rows.append((d, f))
    print(f"{total} pops in {frames} frames of {len(dumps)} dumps")
    if a.ab:
        ab_blocks(a, dumps, rows)
    if a.sheet and rows:
        w, h = Image.open(next(rows[0][0].glob("f*.png"))).size
        sheet = Image.new("RGB", (3 * w, len(rows) * (h + 12)), "white")
        for i, (d, f) in enumerate(rows):
            for j in range(3):
                sheet.paste(Image.open(d / f"f{f + j - 1:06d}.png").convert("RGB"), (j * w, i * (h + 12) + 12))
            ImageDraw.Draw(sheet).text((2, i * (h + 12)), f"{d.name} f{f}", fill="black")
        sheet.save(a.sheet)


if __name__ == "__main__":
    main()
