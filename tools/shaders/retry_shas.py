"""Re-translate the given census sha1s and print each outcome. Usage: retry_shas.py SHA1..."""
import csv
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/shaders"))
import translate as T

idx = {r["sha1"]: r["stage"] for r in csv.DictReader(open(ROOT / "build/shaders/index.csv"))}
jobs = [(s, idx[s], str(ROOT / "build/shaders"), str(ROOT / "build/spirv"), False, T.translator_hash()) for s in sys.argv[1:]]
if __name__ == "__main__":
    with ProcessPoolExecutor(initializer=T.init) as pool:
        for s, r in pool.map(T.work, jobs):
            print(s[:12], r[:150])
