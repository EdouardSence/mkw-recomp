"""The driver's statistics of each specialized pipeline next to its generic pipeline, from a run's log.

Why: With MKW_SPEC_STATS=1 the runtime logs, for every optimised graphics
pipeline, one line per executable with RADV's statistics (instructions, VGPRs, ...) and the stage keys, a variant's key
followed by its generic shader's ("vs=<k>(of <g>)"). This pairs each variant pipeline with the generic pipeline of the
same shaders and compares them per executable.

Usage: uv run tools/shaders/spec_stats.py build/run/NAME.log [--stat Instructions] [--top N]
"""

import argparse
import re
from collections import defaultdict
from pathlib import Path

LINE = re.compile(r"vk_pipeline: stats vs=(\w*)(?:\(of (\w+)\))? fs=(\w*)(?:\(of (\w+)\))? ([^:]+):(.*)")
STAT = re.compile(r" ([^=]+?)=(\d+)")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("log", type=Path)
    ap.add_argument("--stat", default="Instructions", help="statistic to rank by (RADV names, spaces as in the log)")
    ap.add_argument("--top", type=int, default=30)
    args = ap.parse_args()
    generic: dict[tuple[str, str, str], dict[str, int]] = {}
    variants = []
    for ln in args.log.read_text(errors="replace").splitlines():
        m = LINE.search(ln)
        if not m:
            continue
        vs, vs_of, fs, fs_of, exe, stats = m.groups()
        st = {k.replace(" ", ""): int(v) for k, v in STAT.findall(stats)}
        if vs_of or fs_of:
            variants.append(((vs_of or vs, fs_of or fs, exe), (vs, fs), st))
        else:
            generic.setdefault((vs, fs, exe), st)
    stat = args.stat.replace(" ", "")
    rows, tot = [], defaultdict(int)
    for key, (vs, fs), st in variants:
        g = generic.get(key)
        if not g or stat not in st or stat not in g:
            continue
        rows.append((g[stat] - st[stat], key, (vs, fs), g, st))
        tot["pairs"] += 1
        tot["gen"] += g[stat]
        tot["var"] += st[stat]
    rows.sort(key=lambda r: -r[0])
    print(f"{len(variants)} variant executables, {tot['pairs']} with their generic pipeline")
    for diff, (gvs, gfs, exe), _, g, st in rows[: args.top]:
        regs = " ".join(f"{k} {g.get(k)}->{st.get(k)}" for k in ("VGPRs", "SGPRs") if k in g)
        print(f"{gvs} {gfs} {exe[:28]:28} {stat} {g[stat]} -> {st[stat]} ({-diff:+d}) {regs}")
    if tot["gen"]:
        print(f"total {stat}: {tot['gen']} -> {tot['var']} ({tot['var'] / tot['gen']:.1%})")


if __name__ == "__main__":
    main()
