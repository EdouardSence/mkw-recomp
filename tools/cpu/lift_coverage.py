"""Lifter coverage census: which instruction encodings of the game the lifter cannot lift, ranked by how often they occur.

Why: it is the CPU work list. Instead of finding unsupported instructions one crash at a time, every distinct word in
the .text of each module is lifted once and the failures are grouped by mnemonic (capstone) with their counts.

Usage: uv run tools/cpu/lift_coverage.py [--exefs DIR] [--modules main,sdk,subsdk0] [--top N] [--json OUT]
"""

import argparse
import json
import os
import sys
from collections import Counter
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "recomp"))

from lifter import Lifter, Unsupported
from nso import Nso

DEFAULT_EXEFS = Path(os.environ.get("MKW_GAME", "game")) / "exefs"
BASE = 0x8000000  # any aligned base: only PC-relative operands depend on it


def text_words(path: Path) -> np.ndarray:
    nso = Nso(str(path))
    off, size = nso.text
    return np.frombuffer(bytes(nso.image), dtype="<u4", count=size // 4, offset=off)


def lift_chunk(words: list[int]) -> list[tuple[int, str]]:
    """(word, failure) for every word the lifter rejects or turns into a trap."""
    lifter = Lifter(BASE, "c")
    bad = []
    for w in words:
        try:
            ins = lifter.lift(w, BASE)
        except Unsupported as e:
            bad.append((w, f"unsupported: {e}"))
            continue
        except Exception as e:  # noqa: BLE001 -- a lifter bug is coverage data too, not a reason to stop the census
            bad.append((w, f"crash: {type(e).__name__}: {e}"))
            continue
        if ins.kind == "trap":
            bad.append((w, "trap"))
    return bad


def mnemonics(words: list[int]) -> dict[int, str]:
    from capstone import CS_ARCH_ARM64, CS_MODE_ARM, Cs

    cs = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    out = {}
    for w in words:
        ins = next(cs.disasm(w.to_bytes(4, "little"), BASE), None)
        out[w] = ins.mnemonic if ins else "(data?)"
    return out


def census(path: Path, jobs: int) -> tuple[int, Counter, list[tuple[int, str]]]:
    words = text_words(path)
    uniq, counts = np.unique(words, return_counts=True)
    freq = dict(zip(uniq.tolist(), counts.tolist(), strict=True))
    chunks = [uniq[i::jobs].tolist() for i in range(jobs)]
    with ProcessPoolExecutor(jobs) as pool:
        bad = [b for part in pool.map(lift_chunk, chunks) for b in part]
    return len(words), Counter(freq), bad


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--exefs", type=Path, default=DEFAULT_EXEFS)
    ap.add_argument("--modules", default="main,sdk,subsdk0")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--jobs", type=int, default=0, help="worker processes (default: all cores)")
    ap.add_argument("--json", type=Path, help="write the full table here")
    args = ap.parse_args()
    import os

    jobs = args.jobs or os.cpu_count() or 1
    report = {}
    for mod in args.modules.split(","):
        total, freq, bad = census(args.exefs / mod, jobs)
        names = mnemonics([w for w, _ in bad])
        by_kind: Counter = Counter()
        for w, why in bad:
            reason = "trap" if why == "trap" else why.split(":")[0]
            by_kind[(names[w], reason)] += freq[w]
        failed = sum(freq[w] for w, _ in bad)
        print(f"== {mod}: {total:,} words, {len(freq):,} distinct, {failed:,} not lifted ({failed / total:.2%})")
        for (name, reason), n in by_kind.most_common(args.top):
            print(f"{n:>10,}  {name:<12} {reason}")
        report[mod] = {
            "words": total,
            "failed": failed,
            "by_mnemonic": [[name, reason, n] for (name, reason), n in by_kind.most_common()],
            "examples": {names[w]: f"{w:08x} {why}" for w, why in sorted(bad, key=lambda b: -freq[b[0]])},
        }
    if args.json:
        args.json.write_text(json.dumps(report, indent=1))


if __name__ == "__main__":
    main()
