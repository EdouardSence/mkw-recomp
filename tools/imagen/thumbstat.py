# /// script
# dependencies = ["pillow"]
# ///
"""Max and mean luminance of the last thumbnail of each flight dump: thumbstat.py DIR"""
import sys
from pathlib import Path

from PIL import Image, ImageStat

for d in sorted(Path(sys.argv[1]).iterdir()):
    pngs = sorted(d.glob("*.png"))
    if not pngs:
        continue
    for p in (pngs[0], pngs[len(pngs) // 2], pngs[-1]):
        im = Image.open(p).convert("L")
        st = ImageStat.Stat(im)
        print(f"{d.name} {p.name} max {im.getextrema()[1]} mean {st.mean[0]:.1f}")
