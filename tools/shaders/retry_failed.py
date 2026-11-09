"""Re-translate only the programs of failed.tsv whose reason starts with one of the given prefixes; prints outcomes.
Usage: retry_failed.py PREFIX [PREFIX...]"""
import csv
import sys
from collections import Counter
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/shaders"))
import translate as T

idx = {r["sha1"]: r["stage"] for r in csv.DictReader(open(ROOT / "build/shaders/index.csv"))}
shas = [s for line in open(ROOT / "build/spirv/failed.tsv") for reason, rest in [line.rstrip("\n").split("\t", 1)]
        if any(reason.startswith(p) for p in sys.argv[1:]) for s in rest.split()]
jobs = [(s, idx[s], str(ROOT / "build/shaders"), str(ROOT / "build/spirv"), False, T.translator_hash()) for s in shas]
if __name__ == "__main__":
    with ProcessPoolExecutor(initializer=T.init) as pool:
        res = list(pool.map(T.work, jobs))
    for k, n in Counter(r for _, r in res).most_common():
        print(n, k[:150])
