"""Check the operands the SM86 decoder reads against the text nvdisasm prints, on every extracted program.

Why: decode_check.py proves the decoder picks the right instruction; the translator then trusts its fields. This
compares, per instruction, the registers (R, UR), predicates (P, UP, with their `!`), constant-bank operands
(c[bank][offset], offsets modulo 64 KiB) and the `-`/`|x|` source modifiers taken from the decoded fields with the same operands parsed from
the nvdisasm line. Defaults nvdisasm leaves out (RZ, URZ, PT, UPT without `!`) are left out on both sides.
Immediates are not compared.

Usage: uv run tools/shaders/operand_check.py [--shaders build/shaders] [--census build/shader_census] [--spec FILE]
Prints totals and the most frequent disagreements per instruction class with an example of each.
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
LINE = re.compile(r"/\*([0-9a-f]{4,})\*/\s+(.*?)\s*;")
TOKEN = re.compile(
    r"(?P<neg>-)?(?P<abs>\|)?(?P<not>!)?"
    r"(?:(?<![\w.])(?P<reg>U?R)(?P<rn>\d+|Z)\b|(?<![\w.])(?P<pred>U?P)(?P<pn>\d|T)\b"
    r"|c\[(?P<cb>0x[0-9a-f]+)\] ?\[(?:(?P<cr>U?R\d+|U?RZ)(?:\+|(?=\])))?(?P<co>-?0x[0-9a-f]+)?\])"
)
REG_TYPES = {
    "Register": ("R", 255),
    "NonZeroRegister": ("R", 255),
    "ZeroRegister": ("R", 255),
    "UniformRegister": ("UR", 63),
    "NonZeroUniformRegister": ("UR", 63),
}
PRED_TYPES = {"Predicate": "P", "UniformPredicate": "UP"}
_spec = None


def init(spec_path: str) -> None:
    global _spec
    _spec = sass_spec.Spec(spec_path)


def from_text(text: str) -> tuple[Counter, Counter]:
    ops, mods = Counter(), Counter()
    for m in TOKEN.finditer(text):
        if m["reg"]:
            if m["rn"] != "Z":
                ops[f"{m['reg']}{m['rn']}"] += 1
        elif m["pred"]:
            if m["pn"] == "T" and not m["not"]:
                continue
            ops[f"{'!' if m['not'] else ''}{m['pred']}{m['pn']}"] += 1
        else:
            # A constant offset is a 16-bit byte address in the bank; nvdisasm prints the field signed.
            ops[f"c[{int(m['cb'], 16)}][{int(m['co'] or '0', 16) & 0xFFFF}]"] += 1
            if m["cr"] and not m["cr"].endswith("RZ"):
                ops[m["cr"]] += 1
        mods["-"] += bool(m["neg"])
        mods["|"] += bool(m["abs"])
    return ops, mods


def from_fields(d) -> tuple[Counter, Counter]:
    ops, mods = Counter(), Counter()
    f = {iv.item.name: iv for iv in d.values}
    for name, iv in f.items():
        item, v = iv.item, iv.value
        if item.kind != "field" or v is None:
            if item.call_name and item.call_name.startswith("ConstBankAddress") and len(iv.slot_values) == 2:
                bank, addr = (x for _, x in iv.slot_values)
                ops[f"c[{bank}][{(addr * 4 if item.call_name == 'ConstBankAddress2' else addr) & 0xFFFF}]"] += 1
            continue
        if item.format_type in REG_TYPES:
            prefix, zero = REG_TYPES[item.format_type]
            if v != zero:
                ops[f"{prefix}{v}"] += 1
        elif item.format_type in PRED_TYPES:
            neg = f.get(f"{name}@not")
            bang = "!" if neg is not None and neg.value else ""
            if v == 7 and not bang:
                continue
            ops[f"{bang}{PRED_TYPES[item.format_type]}{'T' if v == 7 else v}"] += 1
        elif name.endswith("@negate"):
            mods["-"] += v
        elif name.endswith("@absolute"):
            mods["|"] += v
    return ops, mods


def check(job: tuple[str, str, str, str]) -> tuple[int, Counter, dict]:
    sha, stage, shaders, census = job
    code = Path(shaders, f"{sha}.code").read_bytes()
    if stage != "cs":
        code = code[SPH_SIZE:]
    diffs, examples, n = Counter(), {}, 0
    for line in Path(census, f"{sha}.sass").read_text().splitlines():
        if not (m := LINE.search(line)):
            continue
        off = int(m.group(1), 16)
        d = sass_decode.decode(_spec, int.from_bytes(code[off : off + 16], "little"))
        if d.cls is None:
            continue
        n += 1
        got, ref = from_fields(d), from_text(m.group(2))
        if got != ref:
            extra = (got[0] - ref[0]) + Counter({f"mod{k}": v for k, v in (got[1] - ref[1]).items()})
            miss = (ref[0] - got[0]) + Counter({f"mod{k}": v for k, v in (ref[1] - got[1]).items()})
            key = (d.cls.name, " ".join(sorted(extra)), " ".join(sorted(miss)))
            diffs[key] += 1
            examples.setdefault(key, f"{sha}+{off:#x}: {m.group(2)}")
    return n, diffs, examples


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--shaders", type=Path, default=ROOT / "build/shaders")
    ap.add_argument("--census", type=Path, default=ROOT / "build/shader_census")
    ap.add_argument("--spec", type=Path, default=ROOT / "build/sm86/instructions.json")
    ap.add_argument("--top", type=int, default=40)
    args = ap.parse_args()
    with (args.shaders / "index.csv").open() as f:
        jobs = [(r["sha1"], r["stage"], str(args.shaders), str(args.census)) for r in csv.DictReader(f)]
    total, diffs, examples = 0, Counter(), {}
    with ProcessPoolExecutor(os.cpu_count(), initializer=init, initargs=(str(args.spec),)) as p:
        for n, df, ex in p.map(check, jobs, chunksize=8):
            total += n
            diffs.update(df)
            for k, v in ex.items():
                examples.setdefault(k, v)
    bad = sum(diffs.values())
    print(
        f"{len(jobs)} programs, {total:,} instructions, {total - bad:,} with the same operands ({1 - bad / total:.4%})"
    )
    for (cls, extra, miss), n in diffs.most_common(args.top):
        print(f"{n:>8,}  {cls:<28} decoder+[{extra}] nvdisasm+[{miss}]  e.g. {examples[cls, extra, miss]}")


if __name__ == "__main__":
    main()
