"""Where MKW maps the 64 KiB pages of its sparse textures, against a layout hypothesis.

Why: a sparse texture becomes a Vulkan sparse image whose tiles are bound at each MapVirtual, which needs the page of
the texture's storage -> (layer, level, tile) map the game uses. The game computes its page offsets itself (it calls
only nvnTextureGetSparseTileLayout), so the map is fitted to what it maps.

Input: a run log with MKW_SPARSE_TRACE=1 ("sparse: tex ..." per sparse texture, "sparse: map ..." per request).
Hypothesis (per layer): levels with whole tiles, row-major tiles of 64 KiB each; then the mip tail; the layer stride is
that rounded up to 64 KiB. For each texture it prints the layer stride, how many mapped pages land in each part, and
pages past the end.

Usage: uv run tools/gpu/sparse_fit.py build/run/NAME.log [--tex ADDR]
"""

import argparse
import re
from collections import Counter, defaultdict
from pathlib import Path

PAGE = 0x10000
# NVN format -> (bytes per block, block width, block height); only the formats MKW makes sparse (run-bc7-4)
FORMATS = {0x12: (4, 1, 1), 0x26: (4, 1, 1), 0x32: (2, 1, 1), 0x50: (16, 4, 4), 0x8E: (16, 8, 8)}
SHAPE2D = {1: (256, 256), 2: (256, 128), 4: (128, 128), 8: (128, 64), 16: (64, 64)}

TEX = re.compile(r"sparse: tex (0x[0-9a-f]+) (\d+)x(\d+)x(\d+) lv(\d+) fmt (0x[0-9a-f]+) tgt (\d+) size (0x[0-9a-f]+) "
                 r"layer (0x[0-9a-f]+)")
MAP = re.compile(r"sparse: map (0x[0-9a-f]+) \+(0x[0-9a-f]+) -> (0x[0-9a-f]+)")


def layout(w: int, h: int, levels: int, fmt: int) -> tuple[list[tuple[int, int, int]], int, int]:
    """[(level, tiles_x, tiles_y)] for levels with whole tiles, the tail's byte size, the layer stride."""
    bpb, bw, bh = FORMATS[fmt]
    tw, th = SHAPE2D[bpb]
    tw, th = tw * bw, th * bh
    full, l = [], 0
    while l < levels and max(w >> l, 1) >= tw and max(h >> l, 1) >= th:
        lw, lh = max(w >> l, 1), max(h >> l, 1)
        full.append((l, -(-lw // tw), -(-lh // th)))
        l += 1
    tail = 0
    for k in range(l, levels):
        lw, lh = max(w >> k, 1), max(h >> k, 1)
        tail += -(-lw // bw) * -(-lh // bh) * bpb
    body = sum(tx * ty for _, tx, ty in full) * PAGE
    stride = -(-(body + tail) // PAGE) * PAGE
    return full, tail, stride


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", type=Path)
    ap.add_argument("--tex", help="only the texture at this address; also lists its mapped pages")
    a = ap.parse_args()
    texs, maps = {}, []
    for line in a.log.open(errors="replace"):
        if m := TEX.search(line):
            addr = int(m[1], 16)
            texs[addr] = dict(w=int(m[2]), h=int(m[3]), d=int(m[4]), lv=int(m[5]), fmt=int(m[6], 16), tgt=int(m[7]),
                              size=int(m[8], 16), layer=int(m[9], 16))
        elif m := MAP.search(line):
            maps.append((int(m[1], 16), int(m[2], 16), int(m[3], 16)))
    # newest mapping of each virtual page
    page = {}
    for va, size, pa in maps:
        for k in range(size // PAGE):
            page[va + k * PAGE] = pa + k * PAGE if pa else 0
    print(f"{len(texs)} sparse textures, {len(maps)} requests, {sum(1 for p in page.values() if p)} pages mapped")
    for addr, t in sorted(texs.items()):
        if a.tex and addr != int(a.tex, 16):
            continue
        if t["fmt"] not in FORMATS:
            print(f"{addr:#x}: format {t['fmt']:#x} not in the table")
            continue
        layers = t["d"] if t["tgt"] in (4, 6, 9) else 1
        full, tail, stride = layout(t["w"], t["h"], t["lv"], t["fmt"])
        span = max(t["size"], stride * layers) + 16 * PAGE
        mine = sorted((va - addr) // PAGE for va, pa in page.items() if pa and addr <= va < addr + span)
        parts = Counter()
        per_layer = defaultdict(int)
        for p in mine:
            off = p * PAGE
            layer, rel = divmod(off, stride)
            if layer >= layers:
                parts["past end"] += 1
                continue
            per_layer[layer] += 1
            for lvl, tx, ty in full:
                if rel < tx * ty * PAGE:
                    parts[f"level {lvl}"] += 1
                    break
                rel -= tx * ty * PAGE
            else:
                parts["tail"] += 1
        print(f"{addr:#x} {t['w']}x{t['h']}x{t['d']} lv{t['lv']} fmt {t['fmt']:#x}: ours size {t['size']:#x} layer "
              f"{t['layer']:#x}; hypothesis layer {stride:#x} ({len(full)} levels of tiles "
              f"{[(tx, ty) for _, tx, ty in full]}, tail {tail:#x}); {len(mine)} pages mapped: {dict(parts)}; "
              f"layers with pages {len(per_layer)}")
        if a.tex:
            print(" ".join(f"{p:#x}" for p in mine[:400]))


if __name__ == "__main__":
    main()
