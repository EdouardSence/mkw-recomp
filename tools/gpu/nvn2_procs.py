"""Address of every NVN2 entry point inside the sdk module, from the driver's own proc table.

Why: the driver exports none of its nvn* functions by name; nvnDeviceGetProcAddress finds them by binary search
(sdk+0x5b5a70: names without the "nvn" prefix, sorted, as u32 offsets from the name pool) and returns the pointer at
the same index (sdk+0x5ffad0: a table of RELATIVE-relocated pointers). Reading both tables names ~700 driver functions,
which bindb.py stores so q.py prints them, and which give the size of each opaque object (its SetDefaults).

Offsets below are those of MKW 1.0.0's sdk (build id 90ebba18...); check() fails loudly on another build.

Usage: uv run tools/gpu/nvn2_procs.py   (prints name -> sdk+offset; bindb.py imports procs())
"""

import os
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "recomp"))

from nso import R_AARCH64_RELATIVE, Nso

SDK_BUILD_ID = "90ebba182862b9ecdc21fcb87b99901224fb8f72"
NAME_POOL = 0xB74C26  # adrp/add x25 in sdk+0x5b5a70
NAME_OFFSETS = 0xB79B90  # adrp/add x24 in sdk+0x5b5a70, u32 per entry
LAST_INDEX = 0x2B9  # mov w22, #0x2b9 in sdk+0x5b5a70: highest index of the search
POINTERS = 0xD95168  # adrp/add x8 in sdk+0x5ffad0, u64 per entry


def procs(nso: Nso) -> dict[str, int]:
    """'nvnName' -> offset of the function in the sdk module."""
    if nso.build_id != SDK_BUILD_ID:
        raise ValueError(f"sdk build {nso.build_id}: proc table offsets are for {SDK_BUILD_ID}")
    img = bytes(nso.image)
    relocated = {off: add for off, typ, _, add in nso.relocs if typ == R_AARCH64_RELATIVE}
    out = {}
    for i in range(LAST_INDEX + 1):
        name_at = NAME_POOL + struct.unpack_from("<I", img, NAME_OFFSETS + 4 * i)[0]
        name = img[name_at : img.index(0, name_at)].decode()
        out["nvn" + name] = relocated[POINTERS + 8 * i]
    return out


def main() -> None:
    table = procs(Nso(str(Path(os.environ.get("MKW_GAME", "game")) / "exefs/sdk")))
    for name, off in sorted(table.items(), key=lambda t: t[1]):
        print(f"sdk+{off:#x} {name}")
    print(f"{len(table)} procs", file=sys.stderr)


if __name__ == "__main__":
    main()
