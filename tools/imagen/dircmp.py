# /// script
# dependencies = ["pillow", "numpy"]
# ///
"""Two folders of screenshots of the same frames (two runs, a switch on and off) compared frame by frame.

Why: a change that must not alter the image (MKW_REPLAY_PAR records the frame on several threads) is judged on a wide
range of frames, not one (CONTRIBUTING.md, rendering fixes); a fault of a few frames shows as a peak among neighbours that match.
vsref.py compares one shot with its reference; this runs the same mean error over every name the two folders share.

Usage: uv run tools/imagen/dircmp.py A_DIR B_DIR [--worst N] [--out DIR]
  Prints the mean error (0-1, at 640 wide) of each shared .png, its median and the N worst frames; --out writes each
  of those worst pairs stacked (A on top) and a third row with the difference amplified 4x.
"""

import argparse
import math
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np
from PIL import Image

WIDTH = 640


def load(p: Path) -> np.ndarray:
    im = Image.open(p).convert("RGB")
    return np.asarray(im.resize((WIDTH, round(WIDTH * im.size[1] / im.size[0]))), float) / 255


def error(pair: tuple[Path, Path]) -> float:
    a, b = load(pair[0]), load(pair[1])
    return float(np.abs(a - b).mean()) if a.shape == b.shape else float("nan")


def stacked(a: Path, b: Path, out: Path) -> None:
    A, B = load(a), load(b)
    d = np.clip(np.abs(A - B) * 4, 0, 1)
    Image.fromarray((np.concatenate([A, B, d]) * 255).astype(np.uint8)).save(out)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a", type=Path)
    ap.add_argument("b", type=Path)
    ap.add_argument("--worst", type=int, default=5)
    ap.add_argument("--out", type=Path)
    o = ap.parse_args()

    names = sorted(
        {p.name for p in o.a.glob("*.png") if not p.stem.endswith("-s")} & {p.name for p in o.b.glob("*.png")}
    )
    if not names:
        raise SystemExit(f"no .png shared by {o.a} and {o.b}")
    with ProcessPoolExecutor() as ex:
        errs = list(ex.map(error, [(o.a / n, o.b / n) for n in names]))
    for n, e in zip(names, errs):
        print(f"{n} {e:.4f}")
    ok = [e for e in errs if not math.isnan(e)]
    print(f"{len(names)} frames: median {np.median(ok):.4f}, max {max(ok):.4f}, over 0.01: {sum(e > 0.01 for e in ok)}")
    worst = sorted(zip(errs, names), key=lambda t: 0 if math.isnan(t[0]) else -t[0])[: o.worst]
    print("worst: " + ", ".join(f"{n} {e:.4f}" for e, n in worst))
    if o.out:
        o.out.mkdir(parents=True, exist_ok=True)
        for _, n in worst:
            stacked(o.a / n, o.b / n, o.out / n)
        print(f"wrote {len(worst)} stacked pairs to {o.out}")


if __name__ == "__main__":
    main()
