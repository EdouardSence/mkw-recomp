"""How much of a shader goes away when its stable constant words are fixed: the ceiling of MKW_SPEC without the GPU.

Why: Before any pipeline variant, the census of a run (MKW_CB_CENSUS)
says which fixed-address constant words kept one value in every draw of a shader; this sets those specialization
constants' defaults in its SPIR-V, freezes them and optimises (spirv-opt --set-spec-const-default-value,
--freeze-spec-const, -O), and counts the instructions and conditional branches left in its functions against the
generic SPIR-V optimised the same way. It is SPIR-V, not RADV's code: a proxy for what ACO can drop.

Usage: uv run tools/shaders/spec_ceiling.py CENSUS.tsv [--keys KEY...] [--top N] [--spirv build/spirv]
Without --keys, the N shaders of the census with the most draws (default 40).
"""

import argparse
import csv
import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def body_counts(spv: Path) -> tuple[int, int]:
    """(instructions inside functions, OpBranchConditional + OpSwitch) of a SPIR-V module."""
    dis = subprocess.run(["spirv-dis", "--raw-id", str(spv)], capture_output=True, text=True, check=True).stdout
    n = br = 0
    inside = False
    for ln in dis.splitlines():
        op = next((t for t in ln.split() if t.startswith("Op")), "")
        if op == "OpFunction":
            inside = True
        elif op == "OpFunctionEnd":
            inside = False
        elif inside and op not in ("OpLabel", "OpLine", "OpNoLine"):
            n += 1
            br += op in ("OpBranchConditional", "OpSwitch")
    return n, br


def optimise(src: Path, out: Path, spec: str) -> None:
    args = ["spirv-opt", "--target-env=vulkan1.3"]
    if spec:
        args += ["--set-spec-const-default-value", spec]
    args += ["--freeze-spec-const", "-O", str(src), "-o", str(out)]
    subprocess.run(args, check=True, capture_output=True)


def spec_values(key: str, stable: str, spirv: Path) -> str:
    """'2k:true 2k+1:value ...' for each stable word "bank:byte=value" of the census row."""
    order = {(b, o): k for k, (b, o) in enumerate(json.loads((spirv / f"{key}.res.json").read_text()).get("spec", []))}
    out = []
    for item in stable.split():
        where, _, val = item.partition("=")
        bank, _, byte = where.partition(":")
        k = order.get((int(bank), int(byte, 16)))
        if k is not None:
            out.append(f"{2 * k}:true {2 * k + 1}:{int(val, 16)}")
    return " ".join(out)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("census", type=Path)
    ap.add_argument("--keys", nargs="*")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--spirv", type=Path, default=ROOT / "build/spirv")
    args = ap.parse_args()
    with args.census.open() as f:
        rows = {r["key"]: r for r in csv.DictReader(f, delimiter="\t")}
    keys = args.keys or sorted(rows, key=lambda k: -int(rows[k]["draws"]))[: args.top]
    print("key\tstage\tdraws\twords\tstable\tinsns\tinsns_fixed\tbranches\tbranches_fixed")
    tot = [0, 0, 0, 0]
    with tempfile.TemporaryDirectory(dir=ROOT / "scratchpad") as tmp:
        for key in keys:
            r = rows.get(key)
            src = args.spirv / f"{key}.spv"
            if not r or not src.exists():
                print(f"{key}\tmissing", file=sys.stderr)
                continue
            gen, fix = Path(tmp, "g.spv"), Path(tmp, "f.spv")
            optimise(src, gen, "")
            optimise(src, fix, spec_values(key, r["stable_words"], args.spirv))
            (n0, b0), (n1, b1) = body_counts(gen), body_counts(fix)
            tot = [tot[0] + n0, tot[1] + n1, tot[2] + b0, tot[3] + b1]
            print(f"{key[:16]}\t{r['stage']}\t{r['draws']}\t{r['words']}\t{r['stable']}\t{n0}\t{n1}\t{b0}\t{b1}")
    if tot[0]:
        print(f"total\t\t\t\t\t{tot[0]}\t{tot[1]} ({tot[1] / tot[0]:.1%})\t{tot[2]}\t{tot[3]}")


if __name__ == "__main__":
    main()
