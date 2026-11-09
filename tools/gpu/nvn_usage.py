"""Which NVN2 functions MKW's code uses: the proc-loader's global slots and how many functions read each one.

Why: NVN2 has 699 entry points; implementing only what the game reaches is the work list. The game loads every proc
into a global (nvnDeviceGetProcAddress in one loader function) and calls through those globals, so a slot read from
some other function is a use. A lower bound: code that reaches a slot through a pointer to the whole table is not seen.

Usage: uv run tools/gpu/nvn_usage.py [--db build/bindb.sqlite] [--out build/nvn_usage.csv] [--nvn1 FILE]
--nvn1: NVN1 proc names, one per line (default nvn1_procs.txt here, from totk-pc-recomp) to mark the new ones.
"""

import argparse
import csv
import sqlite3
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def loader(db: sqlite3.Connection) -> int:
    """The main function that references the most nvn* names: the proc loader."""
    row = db.execute(
        "SELECT r.func FROM ref r JOIN str s ON s.addr=r.target "
        "WHERE s.module='main' AND s.text LIKE 'nvn%' GROUP BY r.func ORDER BY count(*) DESC LIMIT 1"
    ).fetchone()
    return row[0]


def slots(db: sqlite3.Connection, func: int) -> dict[str, int]:
    """name -> global slot: each name's string reference pairs with the next store-like reference after it."""
    end = db.execute("SELECT end FROM func WHERE addr=?", (func,)).fetchone()[0]
    rows = db.execute(
        "SELECT r.target, r.kind, s.text FROM ref r LEFT JOIN str s ON s.addr=r.target "
        "WHERE r.site>=? AND r.site<? ORDER BY r.site",
        (func, end),
    )
    out, pending = {}, None
    for target, kind, text in rows:
        if text and text.startswith("nvn"):
            pending = text
        elif kind == "ldst" and pending:
            out[pending] = target
            pending = None
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--db", type=Path, default=ROOT / "build/bindb.sqlite")
    ap.add_argument("--out", type=Path, default=ROOT / "build/nvn_usage.csv")
    ap.add_argument("--nvn1", type=Path, default=Path(__file__).with_name("nvn1_procs.txt"))
    args = ap.parse_args()
    db = sqlite3.connect(args.db)
    func = loader(db)
    users = Counter()
    for name, slot in slots(db, func).items():
        users[name] = db.execute(
            "SELECT count(DISTINCT func) FROM ref WHERE target=? AND func!=?", (slot, func)
        ).fetchone()[0]
    nvn1 = {n for n in args.nvn1.read_text().split() if n.startswith("nvn")}
    with args.out.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(("name", "functions", "in_nvn1"))
        w.writerows((n, c, int(n in nvn1)) for n, c in users.most_common())
    used = [n for n, c in users.items() if c]
    new = sorted(n for n in used if n not in nvn1)
    base = db.execute("SELECT base FROM module WHERE name='main'").fetchone()[0]
    print(f"loader main+{func - base:#x}: {len(users)} slots, {len(used)} read elsewhere")
    print(f"used and in NVN1: {len(used) - len(new)}; used and new in NVN2: {len(new)}")
    print("  " + " ".join(new))


if __name__ == "__main__":
    main()
