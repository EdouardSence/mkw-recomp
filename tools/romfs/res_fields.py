"""Print every scalar field of GamePerformanceGraphicsConfigGParam in the original archive and in an overlay."""
import os
import struct
import sys
from compression import zstd
from pathlib import Path

sys.path.insert(0, "tools/romfs"); sys.path.insert(0, "tools/shaders")
from extract import sarc_members
from resolution import ARCHIVE, MEMBER, byml_root_fields


def fields(p):
    s = zstd.decompress(Path(p).read_bytes()); m = sarc_members(s)[MEMBER]; at = s.index(m)
    out = {}
    for k, (off, t) in byml_root_fields(s, at).items():
        v = struct.unpack_from("<I", s, off)[0]
        if t == 0xD2: v = round(struct.unpack_from("<f", s, off)[0], 4)
        out[k] = (t, v)
    return out
a = fields(Path(os.environ.get("MKW_GAME", "game")) / "romfs" / ARCHIVE); b = fields(Path("build/romfs-res") / ARCHIVE)
for k in sorted(a): print(f"{k:60s} {a[k][0]:#x} {a[k][1]!s:>10} {'-> ' + str(b[k][1]) if b[k] != a[k] else ''}")
