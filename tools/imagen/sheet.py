# /// script
# dependencies = ["pillow"]
# ///
"""Contact sheet of flight thumbnails: sheet.py OUT.png STEP DIR... (every STEP-th frame, labelled)"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw

out, step, dirs = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
seen = {}
for d in dirs:
    for p in Path(d).rglob("f*.png"):
        seen.setdefault(int(p.stem[1:]), p)
frames = [f for f in sorted(seen) if f % step == 0]
cols, w, h = 8, 320, 180
sheet = Image.new("RGB", (cols * w, (len(frames) + cols - 1) // cols * (h + 14)), "black")
dr = ImageDraw.Draw(sheet)
for i, f in enumerate(frames):
    x, y = i % cols * w, i // cols * (h + 14)
    sheet.paste(Image.open(seen[f]).convert("RGB"), (x, y + 14))
    dr.text((x + 2, y), str(f), fill="yellow")
sheet.save(out)
print(len(frames), "frames", frames[0], frames[-1])
