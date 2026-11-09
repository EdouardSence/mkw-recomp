"""Classify dumped programs (MKW_SPIRV_DUMP) as in the census (index.csv) or not. Usage: classify_missing.py DIR"""
import csv
import hashlib
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/shaders"))
from extract import sph_stage, trim

idx = {r["sha1"]: r for r in csv.DictReader(open(ROOT / "build/shaders/index.csv"))}
for code in sorted(Path(sys.argv[1]).glob("*.code")):
    raw = code.read_bytes(); ctl = code.with_suffix(".ctl").read_bytes()
    st = sph_stage(raw) or "cs"
    t = trim(raw, st)
    sha = hashlib.sha1(ctl + t).hexdigest()
    r = idx.get(sha)
    print(code.stem[:16], st, len(t), sha, r["source"] if r else "NOT-IN-CENSUS")
