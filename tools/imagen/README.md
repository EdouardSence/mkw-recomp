# tools/imagen

Reading the flight recorder thumbnails (`MKW_FLIGHT`) and frame dumps, and comparing against the reference.

| Script | What it does | Usage |
|---|---|---|
| `sheet.py` | Contact sheet of thumbnails, one every N frames with its number | `uv run tools/imagen/sheet.py OUT.png N DIR...` |
| `pops.py` | Isolated one-frame jumps (something that appears or goes missing for a single frame: the tree flicker), in 10x10 blocks; not all are bugs (an animated sign jumps too): they are reviewed on a sheet | `uv run tools/imagen/pops.py DIR... [--min N] [--sheet OUT.png] [--ab LOG]` (`--ab`: jumps per `MKW_AB` block) |
| `thumbcut.py` | Frames where the thumbnail goes from black to having an image or back | `uv run tools/imagen/thumbcut.py DIR` |
| `thumbstat.py` | Maximum and mean luminance of the last thumbnail of each dump | `uv run tools/imagen/thumbstat.py DIR` |
| `vsref.py` | A capture against its reference (a romfs video through ffmpeg or a console image): the most similar frame, mean error and the error if sRGB were applied twice, and the two stacked | `uv run tools/imagen/vsref.py CAPTURE REF [--out X.png]` |
| `fg_check.py` | Frame generation without the game: the frame `mkw --probe-framegen` makes between A and C against the real B (three consecutive frames from `MKW_SHOTS`), or a picture moved by a known shift under a still box; PSNR of the made frame, the plain blend and A held, over the whole frame and where it moves, and a sheet | `uv run tools/imagen/fg_check.py A.png B.png C.png` or `--synthetic IMG [--shift 24,6]` |
| `dircmp.py` | Two folders of captures of the same frames (two runs, a switch on and off): mean error of each frame, median, worst ones and their pairs stacked with the difference | `uv run tools/imagen/dircmp.py A B [--worst N] [--out DIR]` |
