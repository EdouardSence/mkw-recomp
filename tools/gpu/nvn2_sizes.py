"""Lower bound of each NVN2 object's size, from the offsets the driver's own functions touch, next to NVN1's size.

Why: the NVN front end taken from totk-pc-recomp keeps its state inside the game's opaque NVN objects (FITS() checks
against NVN1 sizes). NVN2 objects are allocated by MKW with NVN2 sizes; this shows which grew. Each nvn<Type>* entry
point of the sdk (named by nvn2_procs.py) is scanned from its start until x0, the object pointer, is overwritten or
the function returns: the largest [x0, #imm] access end is a lower bound of sizeof(NVN<type>).

Usage: uv run tools/gpu/nvn2_sizes.py [--nvn-h PATH]   (NVN1 sizes from totk-pc-recomp runtime/nvn.h, MIT)
"""

import argparse
import os
import re
import sys
from collections import Counter
from pathlib import Path

from capstone import CS_ARCH_ARM64, CS_MODE_ARM, Cs
from capstone.arm64 import ARM64_OP_MEM, ARM64_REG_W0, ARM64_REG_X0

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "recomp"))
sys.path.insert(0, str(Path(__file__).parent))

from nso import Nso
from nvn2_procs import procs

DEFAULT_NVN_H = ROOT / "scratchpad/ext/totk/runtime/nvn.h"
RESERVED = re.compile(r"typedef struct \{[^}]*?reserved\[(0x[0-9a-fA-F]+|\d+)\][^}]*\}\s*(NVN\w+);")
SCAN = 0x400  # bytes scanned per entry point; accessors are short


def access_size(mnemonic: str, first_reg: str) -> int:
    size = {"q": 16, "x": 8, "d": 8, "w": 4, "s": 4, "h": 2, "b": 1}.get(first_reg[:1], 8)
    if mnemonic.endswith(("b", "rb")) and mnemonic[:3] in ("ldr", "str", "stu", "ldu"):
        size = 1
    elif mnemonic.endswith("h") and mnemonic[:3] in ("ldr", "str", "stu", "ldu"):
        size = 2
    return size * 2 if mnemonic in ("stp", "ldp") else size


def lower_bounds(sdk: Nso, types: list[str]) -> Counter:
    img = bytes(sdk.image)
    cs = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    cs.detail = True
    bound: Counter = Counter()
    by_prefix = sorted(types, key=len, reverse=True)  # NVNtextureBuilder before NVNtexture
    for name, at in procs(sdk).items():
        t = next((t for t in by_prefix if name.startswith("nvn" + t[3:4].upper() + t[4:])), None)
        if not t:
            continue
        for ins in cs.disasm(img[at : at + SCAN], at):
            for op in ins.operands:
                if op.type == ARM64_OP_MEM and op.mem.base == ARM64_REG_X0 and not op.mem.index:
                    end = op.mem.disp + access_size(ins.mnemonic, ins.op_str)
                    bound[t] = max(bound[t], end)
            _, written = ins.regs_access()
            if ARM64_REG_X0 in written or ARM64_REG_W0 in written or ins.mnemonic in ("ret", "b", "br", "bl", "blr"):
                break
    return bound


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--nvn-h", type=Path, default=DEFAULT_NVN_H)
    ap.add_argument("--exefs", type=Path, default=Path(os.environ.get("MKW_GAME", "game")) / "exefs")
    args = ap.parse_args()
    nvn1 = {m.group(2): int(m.group(1), 0) for m in RESERVED.finditer(args.nvn_h.read_text())}
    bound = lower_bounds(Nso(str(args.exefs / "sdk")), list(nvn1))
    print(f"{'type':<24} {'NVN1':>7} {'NVN2 >=':>8}")
    for t in sorted(nvn1):
        if bound[t]:
            mark = "  grows" if bound[t] > nvn1[t] else ""
            print(f"{t:<24} {nvn1[t]:>#7x} {bound[t]:>#8x}{mark}")


if __name__ == "__main__":
    main()
