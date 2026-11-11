# /// script
# dependencies = ["pillow", "numpy"]
# ///
"""Frame generation without the game: how close a made frame is to the real one.

Why: a game run takes minutes and its frames are never the same twice; frame generation's passes
(runtime/gfx/vk/device/vk_framegen.c) run here on the GPU in seconds, through `mkw --probe-framegen`, on frames whose
middle frame is known. A change to the shaders or their constants is judged by these numbers before any game run.

Two cases:
  three consecutive frames A B C (MKW_SHOTS=lo-hi:1 with MKW_SHOTS_FULL=1 saves them): the frame made between A and C
      against B;
  --synthetic IMG: IMG moved by --shift pixels from A to C (B halfway) under a still box standing in for the HUD, so the
      motion is known exactly and the box must not move.
Prints the PSNR (dB, higher is better) of the made frame, of the plain blend of A and C and of A held, over the whole
frame and over the pixels that change between A and C; writes A, the made frame, B and the made frame's error stacked
into OUT/sheet.png. Pictures stay in OUT (build/, never in git).

Usage: uv run tools/imagen/fg_check.py A.png B.png C.png [--out build/fg] [--mkw build/dist/linux/mkw]
       uv run tools/imagen/fg_check.py --synthetic IMG [--shift 24,6] [--out build/fg]
Environment (MKW_FG_LAMBDA, MKW_FG_BIAS) reaches the probe.
"""

import argparse
import os
import struct
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]


def load(path: str) -> np.ndarray:
    return np.asarray(Image.open(path).convert("RGBA"), np.uint8)


def write_rgba(path: Path, px: np.ndarray) -> None:
    h, w = px.shape[:2]
    path.write_bytes(struct.pack("<II", w, h) + np.ascontiguousarray(px).tobytes())


def shifted(img: np.ndarray, dx: float, dy: float) -> np.ndarray:
    """img moved by (dx, dy) pixels, edges clamped, bilinear for half pixels."""
    h, w = img.shape[:2]
    ys, xs = np.mgrid[0:h, 0:w].astype(np.float32)
    sx, sy = np.clip(xs - dx, 0, w - 1), np.clip(ys - dy, 0, h - 1)
    x0, y0 = np.floor(sx).astype(int), np.floor(sy).astype(int)
    x1, y1 = np.minimum(x0 + 1, w - 1), np.minimum(y0 + 1, h - 1)
    fx, fy = (sx - x0)[..., None], (sy - y0)[..., None]
    f = img.astype(np.float32)
    top = f[y0, x0] * (1 - fx) + f[y0, x1] * fx
    bot = f[y1, x0] * (1 - fx) + f[y1, x1] * fx
    return np.round(top * (1 - fy) + bot * fy).astype(np.uint8)


def with_hud(img: np.ndarray) -> np.ndarray:
    """A still box with stripes, where the game's HUD would be."""
    out = img.copy()
    h, w = img.shape[:2]
    y0, x0 = h // 12, w // 16
    box = out[y0 : y0 + h // 8, x0 : x0 + w // 4]
    box[:] = (230, 230, 230, 255)
    box[:, ::7] = (20, 20, 20, 255)
    return out


def psnr(a: np.ndarray, b: np.ndarray, mask: np.ndarray | None = None) -> float:
    d = (a[..., :3].astype(np.float64) - b[..., :3].astype(np.float64)) ** 2
    if mask is not None:
        if not mask.any():
            return float("nan")
        d = d[mask]
    mse = d.mean()
    return 99.0 if mse == 0 else 10 * np.log10(255**2 / mse)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("frames", nargs="*", help="A.png B.png C.png")
    ap.add_argument("--synthetic", help="one picture, moved by --shift")
    ap.add_argument("--shift", default="24,6", help="dx,dy in pixels from A to C (--synthetic)")
    ap.add_argument("--out", default=str(ROOT / "build/fg"))
    ap.add_argument("--mkw", default=str(ROOT / "build/dist/linux/mkw"))
    args = ap.parse_args()
    if args.synthetic:
        dx, dy = (float(v) for v in args.shift.split(","))
        img = load(args.synthetic)
        a, b, c = (with_hud(shifted(img, dx * k, dy * k)) for k in (0, 0.5, 1))
    elif len(args.frames) == 3:
        a, b, c = (load(f) for f in args.frames)
    else:
        ap.error("three frames, or --synthetic IMG")
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    write_rgba(out / "a.rgba", a)
    write_rgba(out / "b.rgba", c)
    env = dict(os.environ, MKW_FG_PROBE=str(out))
    r = subprocess.run([args.mkw, "--probe-framegen"], env=env, capture_output=True, text=True)
    print("\n".join(line for line in r.stdout.splitlines() if line.startswith("probe-framegen")))
    if r.returncode:
        print(r.stderr[-2000:], file=sys.stderr)
        sys.exit(1)
    made = load(str(out / "mid.png"))
    blend = ((a.astype(np.uint16) + c) // 2).astype(np.uint8)
    moving = np.abs(a[..., :3].astype(int) - c[..., :3]).max(axis=2) > 8
    print(f"{'':12}{'whole':>8}{'moving':>8}   (PSNR dB against B; {moving.mean() * 100:.0f}% of the pixels move)")
    for name, x in (("made", made), ("blend", blend), ("A held", a)):
        print(f"{name:12}{psnr(x, b):8.2f}{psnr(x, b, moving):8.2f}")
    err = np.clip(np.abs(made[..., :3].astype(int) - b[..., :3]) * 4, 0, 255).astype(np.uint8)
    w = 960
    tiles = [Image.fromarray(x[..., :3]) for x in (a, made, b)] + [Image.fromarray(err)]
    tiles = [t.resize((w, round(t.height * w / t.width))) for t in tiles]
    sheet = Image.new("RGB", (w, sum(t.height for t in tiles)))
    y = 0
    for t in tiles:
        sheet.paste(t, (0, y))
        y += t.height
    sheet.save(out / "sheet.png")
    print(f"sheet (A, made, B, error x4): {out / 'sheet.png'}")


if __name__ == "__main__":
    main()
