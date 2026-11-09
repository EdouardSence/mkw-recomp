"""Opcode census of the extracted shaders: which SM86 instructions MKW uses, in how many programs and how often.

Why: it is the shader translator's work list, ranked, before writing any of it. NVIDIA's nvdisasm (CUDA 12.8, local
only, never in the repo) is the reference decoder here; the translator itself decodes with the spec generated from
DocumentSASS / nvsass-disassembler, and this census is also its test oracle.

Usage: uv run tools/shaders/census.py [--shaders build/shaders] [--nvdisasm PATH] [--top N] [--out build/shader_census]
Writes <out>/<sha1>.sass (disassembly) and prints the ranking; failures to disassemble are listed too.
"""

import argparse
import csv
import os
import re
import subprocess
from collections import Counter
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_NVDISASM = ROOT / "scratchpad/nvidia/cuda/bin/nvdisasm"
SPH_SIZE = 0x80
ILLEGAL_AT = re.compile(r"at address 0x([0-9a-f]+)")
INSN = re.compile(r"/\*[0-9a-f]{4,}\*/\s+(?:@!?U?P[T0-9]+\s+)?([A-Z][A-Z0-9_]*)((?:\.[A-Z0-9_]+)*)")


def disassemble(job: tuple[Path, str, Path, Path]) -> tuple[str, Counter, str]:
    """(sha1, opcode counts, error) for one stage; the disassembly is kept next to the census."""
    code_path, stage, nvdisasm, out = job
    code = code_path.read_bytes()
    if stage != "cs":
        code = code[SPH_SIZE:]
    raw = out / f"{code_path.stem}.bin"
    while True:
        raw.write_bytes(code)
        res = subprocess.run([str(nvdisasm), "-b", "SM86", str(raw)], capture_output=True, text=True, check=False)
        # nvdisasm rejects the whole program for one illegal word: data the extractor left after the code (183 of the
        # 11 561, 06/11/2026). Cut there and disassemble again; the decoder stops at that word too.
        bad = ILLEGAL_AT.search(res.stderr)
        if not bad or not 0 < int(bad.group(1), 16) < len(code):
            break
        code = code[: int(bad.group(1), 16)]
    raw.unlink()
    (out / f"{code_path.stem}.sass").write_text(res.stdout)
    ops: Counter = Counter()
    for line in res.stdout.splitlines():
        if m := INSN.search(line):
            ops[m.group(1)] += 1
    err = res.stderr.strip().splitlines()[0] if res.returncode or not ops else ""
    return code_path.stem, ops, err


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--shaders", type=Path, default=ROOT / "build/shaders")
    ap.add_argument("--nvdisasm", type=Path, default=DEFAULT_NVDISASM)
    ap.add_argument("--out", type=Path, default=ROOT / "build/shader_census")
    ap.add_argument("--top", type=int, default=80)
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    with (args.shaders / "index.csv").open() as f:
        index = list(csv.DictReader(f))
    jobs = [(args.shaders / f"{r['sha1']}.code", r["stage"], args.nvdisasm, args.out) for r in index]
    total, programs, failed = Counter(), Counter(), []
    with ProcessPoolExecutor(os.cpu_count()) as pool:
        for sha, ops, err in pool.map(disassemble, jobs, chunksize=16):
            total.update(ops)
            programs.update(ops.keys())
            if err:
                failed.append((sha, err))
    print(f"{len(jobs)} stages, {sum(total.values()):,} instructions, {len(total)} opcodes, {len(failed)} failed")
    print(f"{'opcode':<10} {'count':>10} {'programs':>9}")
    for op, n in total.most_common(args.top):
        print(f"{op:<10} {n:>10,} {programs[op]:>9}")
    for sha, err in failed[:20]:
        print(f"FAILED {sha}: {err}")
    with (args.out / "opcodes.csv").open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(("opcode", "count", "programs"))
        w.writerows((op, n, programs[op]) for op, n in total.most_common())


if __name__ == "__main__":
    main()
