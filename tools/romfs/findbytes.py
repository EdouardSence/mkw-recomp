"""Find given byte needles in every romfs file (zstd decompressed, recursively into nested zstd)."""
import multiprocessing
import os
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import zstandard as zstd

NEEDLES = [bytes.fromhex(h) for h in sys.argv[2:]]

def look(path):
    try:
        data = Path(path).read_bytes()
    except OSError:
        return []
    out, layers = [], [data]
    while layers:
        d = layers.pop()
        if d[:4] == b"\x28\xb5\x2f\xfd":
            try:
                layers.append(zstd.ZstdDecompressor().decompress(d, max_output_size=1 << 31))
            except zstd.ZstdError:
                pass
            continue
        for k, n in enumerate(NEEDLES):
            at = d.find(n)
            if at >= 0:
                out.append((path, k, at))
        pos = 4
        while (pos := d.find(b"\x28\xb5\x2f\xfd", pos)) > 0 and len(layers) < 64:  # zstd frames inside archives
            try:
                layers.append(zstd.ZstdDecompressor().decompressobj().decompress(d[pos:]))
            except zstd.ZstdError:
                pass
            pos += 4
    return out

files = [str(p) for p in Path(sys.argv[1]).rglob("*") if p.is_file()]
with ProcessPoolExecutor(os.cpu_count(), mp_context=multiprocessing.get_context("fork")) as pool:
    for res in pool.map(look, files, chunksize=32):
        for r in res:
            print(*r, flush=True)
