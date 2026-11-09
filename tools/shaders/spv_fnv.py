"""Map dump hashes (FNV-1a 64 of the SPIR-V bytes, vk_dump.c shader_hash) to program keys in build/spirv."""
import sys
from pathlib import Path

want = set(sys.argv[1:])
for p in Path("build/spirv").glob("*.spv"):
    h = 0xcbf29ce484222325
    for b in p.read_bytes():
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    if f"{h:016x}" in want: print(f"{h:016x} {p.stem}")
