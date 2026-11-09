"""Check the SM86 decoder (sm86/decode, spec from tools/shaders/spec.sh) against nvdisasm on every extracted program.

Why: the translator trusts the decoder's class and fields; this proves, for each instruction MKW actually contains,
that the decoder picks the instruction nvdisasm prints. Reference disassembly comes from tools/shaders/census.py.

Usage: uv run tools/shaders/decode_check.py [--shaders build/shaders] [--census build/shader_census] [--spec DIR]
Prints totals, the decoder status counts and the most frequent disagreements with an example of each.
"""

import argparse
import csv
import os
import re
import sys
from collections import Counter
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "sm86/decode"))

import sass_decode
import sass_spec

SPH_SIZE = 0x80
INSN = re.compile(r"/\*([0-9a-f]{4,})\*/\s+(?:@!?U?P[T0-9]+\s+)?([A-Z][A-Z0-9_]*)")
_spec = None


def init(spec_path: str) -> None:
    global _spec
    _spec = sass_spec.Spec(spec_path)


def check(job: tuple[str, str, str, str]) -> tuple[int, Counter, Counter, dict]:
    sha, stage, shaders, census = job
    code = Path(shaders, f"{sha}.code").read_bytes()
    if stage != "cs":
        code = code[SPH_SIZE:]
    status, diffs, examples, n = Counter(), Counter(), {}, 0
    for line in Path(census, f"{sha}.sass").read_text().splitlines():
        if not (m := INSN.search(line)):
            continue
        off, ref = int(m.group(1), 16), m.group(2)
        d = sass_decode.decode(_spec, int.from_bytes(code[off : off + 16], "little"))
        n += 1
        status[d.status] += 1
        if d.mnemonic != ref:
            key = (d.mnemonic or d.status, ref)
            diffs[key] += 1
            examples.setdefault(key, f"{sha}+{off:#x}")
    return n, status, diffs, examples


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--shaders", type=Path, default=ROOT / "build/shaders")
    ap.add_argument("--census", type=Path, default=ROOT / "build/shader_census")
    ap.add_argument("--spec", type=Path, default=ROOT / "build/sm86/instructions.json")
    args = ap.parse_args()
    with (args.shaders / "index.csv").open() as f:
        jobs = [(r["sha1"], r["stage"], str(args.shaders), str(args.census)) for r in csv.DictReader(f)]
    total, status, diffs, examples = 0, Counter(), Counter(), {}
    with ProcessPoolExecutor(os.cpu_count(), initializer=init, initargs=(str(args.spec),)) as p:
        for n, st, df, ex in p.map(check, jobs, chunksize=8):
            total += n
            status.update(st)
            diffs.update(df)
            for k, v in ex.items():
                examples.setdefault(k, v)
    agree = total - sum(diffs.values())
    print(f"{len(jobs)} programs, {total:,} instructions, {agree:,} agree with nvdisasm ({agree / total:.4%})")
    print("decoder status: " + ", ".join(f"{k} {v:,}" for k, v in status.most_common()))
    for (got, ref), n in diffs.most_common(20):
        print(f"{n:>8,}  decoder {got:<14} nvdisasm {ref:<10} e.g. {examples[got, ref]}")


if __name__ == "__main__":
    main()
