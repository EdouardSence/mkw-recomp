"""Minimal little-endian BYML reader (v2-v7 common nodes) printing the tree. Usage: byml.py FILE"""
import struct
import sys

d = open(sys.argv[1], "rb").read()
assert d[:2] == b"YB", d[:4]
ver, keys_off, strs_off, root_off = struct.unpack_from("<HIII", d, 2)

def table(off):
    if not off:
        return []
    n = struct.unpack_from("<I", d, off)[0] >> 8
    offs = struct.unpack_from(f"<{n + 1}I", d, off + 4)
    return [d[off + o: d.index(0, off + o)].decode() for o in offs[:n]]

keys, strs = table(keys_off), table(strs_off)

def val(t, v):
    if t == 0xA0: return strs[v]
    if t == 0xD0: return bool(v)
    if t == 0xD1: return struct.unpack("<i", struct.pack("<I", v))[0]
    if t == 0xD2: return struct.unpack("<f", struct.pack("<I", v))[0]
    if t == 0xD3: return v
    if t in (0xD4, 0xD5, 0xD6): return struct.unpack_from("<q" if t == 0xD4 else "<Q" if t == 0xD5 else "<d", d, v)[0]
    if t == 0xFF: return None
    if t in (0xC0, 0xC1): return node(v)
    return f"<type {t:#x} {v:#x}>"

def node(off):
    t, n = d[off], int.from_bytes(d[off + 1: off + 4], "little")
    if t == 0xC1:
        out = {}
        for k in range(n):
            e = off + 4 + 8 * k
            ki, et, v = int.from_bytes(d[e:e + 3], "little"), d[e + 3], struct.unpack_from("<I", d, e + 4)[0]
            out[keys[ki]] = val(et, v)
        return out
    if t == 0xC0:
        types = d[off + 4: off + 4 + n]
        base = off + 4 + ((n + 3) & ~3)
        return [val(types[k], struct.unpack_from("<I", d, base + 4 * k)[0]) for k in range(n)]
    return f"<node {t:#x}>"

import json

print(f"version {ver}")
print(json.dumps(node(root_off), indent=1, default=str))
