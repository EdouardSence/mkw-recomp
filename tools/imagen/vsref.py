# /// script
# dependencies = ["pillow", "numpy"]
# ///
"""A screenshot of the game against its reference: a movie of the romfs (decoded by ffmpeg) or a console picture.

Why: the image is judged on the final screenshot against the reference (CONTRIBUTING.md, rendering fixes), and the comparison
was rewritten by hand eight times in two sessions (31/10 and 01/11/2026): find the movie frame that matches, the mean
error, and the error if sRGB were applied twice (the grey movie). This does it in one call.

Usage: uv run tools/imagen/vsref.py SHOT.png REF [--out SIDE.png]
  REF: a movie (.webm, .mp4: frames at 10 fps searched by structure, then the best one decoded at full size; BT.709
       limited range, as the game's movies are) or an image.
  --out: both pictures stacked (shot on top), 1280 wide, to look at.
"""

import argparse
import subprocess
from pathlib import Path

import numpy as np
from PIL import Image

SMALL = (320, 180)
SCALE = "in_color_matrix=bt709:in_range=tv:out_range=pc"


def rgb(img: Image.Image, size: tuple[int, int]) -> np.ndarray:
    return np.asarray(img.convert("RGB").resize(size), float) / 255


def structure(a: np.ndarray) -> np.ndarray:
    """Grey with mean and contrast removed: a frame matches by its shapes, whatever the colour error."""
    g = a.mean(2)
    return (g - g.mean()) / (g.std() + 1e-6)


def movie_frames(path: Path) -> np.ndarray:
    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", str(path), "-vf", f"fps=10,scale={SMALL[0]}:{SMALL[1]}:{SCALE}",
         "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
        capture_output=True, check=True,
    ).stdout  # fmt: skip
    return np.frombuffer(raw, np.uint8).reshape(-1, SMALL[1], SMALL[0], 3).astype(float) / 255


def movie_frame(path: Path, seconds: float, size: tuple[int, int]) -> Image.Image:
    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-ss", f"{seconds:.2f}", "-i", str(path), "-frames:v", "1",
         "-vf", f"scale={size[0]}:{size[1]}:{SCALE}", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
        capture_output=True, check=True,
    ).stdout  # fmt: skip
    return Image.frombytes("RGB", size, raw[: size[0] * size[1] * 3])


def srgb_encode(x: np.ndarray) -> np.ndarray:
    return np.where(x <= 0.0031308, 12.92 * x, 1.055 * np.power(np.clip(x, 0, None), 1 / 2.4) - 0.055)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("shot", type=Path)
    ap.add_argument("ref", type=Path)
    ap.add_argument("--out", type=Path)
    a = ap.parse_args()

    shot = Image.open(a.shot).convert("RGB")
    if a.ref.suffix.lower() in {".webm", ".mp4", ".mkv"}:
        frames = movie_frames(a.ref)
        s = structure(rgb(shot, SMALL))
        scores = [(structure(f) * s).mean() for f in frames]
        best = int(np.argmax(scores))
        print(f"best match: {best / 10:.1f} s of {len(frames) / 10:.1f} (correlation {scores[best]:.3f})")
        ref = movie_frame(a.ref, best / 10, shot.size)
    else:
        ref = Image.open(a.ref).convert("RGB").resize(shot.size)

    size = (640, round(640 * shot.size[1] / shot.size[0]))
    S, R = rgb(shot, size), rgb(ref, size)
    print(
        f"mean error {np.abs(S - R).mean():.3f} (0-1); if sRGB were applied twice {np.abs(S - srgb_encode(R)).mean():.3f}"
    )
    print("mean RGB shot " + " ".join(f"{v:.3f}" for v in S.mean((0, 1)))
          + ", ref " + " ".join(f"{v:.3f}" for v in R.mean((0, 1))))  # fmt: skip
    if a.out:
        w, h = 1280, round(1280 * shot.size[1] / shot.size[0])
        side = Image.new("RGB", (w, 2 * h))
        side.paste(shot.resize((w, h)), (0, 0))
        side.paste(ref.resize((w, h)), (0, h))
        side.save(a.out)
        print(f"wrote {a.out}")


if __name__ == "__main__":
    main()
