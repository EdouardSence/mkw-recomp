# /// script
# dependencies = ["pillow"]
# ///
"""Frames of a flight dump where the thumbnail's max luminance changes between zero and non-zero: thumbcut.py DIR"""
import sys
from pathlib import Path

from PIL import Image

prev = None
for p in sorted(Path(sys.argv[1]).glob("*.png")):
    m = Image.open(p).convert("L").getextrema()[1]
    if prev is None or (m == 0) != (prev == 0):
        print(p.name, "max", m)
    prev = m
