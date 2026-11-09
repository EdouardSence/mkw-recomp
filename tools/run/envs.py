"""Catalogue of the run's settings: every MKW_* name the runtime reads, where, and the comment next to it.

Why: about 200 variables, read through cfg_get (runtime/core/base/cfg.h) or rt_knob all over runtime/, and some of the most
used (MKW_FLIGHT_AT, MKW_FLIGHT_SPIKE_MS) are in no README. Generated from the code each time, so it cannot go stale.

Usage: uv run tools/run/envs.py [REGEX]   e.g. envs.py FLIGHT, envs.py 'SEQ|CTL'
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NAME = re.compile(r'"(MKW_[A-Z0-9_]+)"')
KNOB = re.compile(r'rt_knob\("([A-Z0-9_]+)"')  # rt_knob prefixes MKW_ itself (runtime/core/diag/perf.c)


def comment(lines: list[str], i: int) -> str:
    """The trailing // comment of line i, else the // block right above it."""
    if "//" in lines[i]:
        return lines[i].split("//", 1)[1].strip()
    above = []
    j = i - 1
    while j >= 0 and lines[j].strip().startswith("//") and len(above) < 4:
        above.insert(0, lines[j].strip()[2:].strip())
        j -= 1
    return " ".join(above)


def doc_mention(lines: list[str], name: str) -> str:
    """Where the file's comments describe name (often its header): from the name on, with the comment lines after."""
    for i, line in enumerate(lines):
        text = line.strip()
        if text.startswith("//") and re.search(rf"\b{name}\b", text):
            out = text[text.index(name) :]
            for nxt in lines[i + 1 : i + 4]:
                if not nxt.strip().startswith("//") or len(out) > 220:
                    break
                out += " " + nxt.strip()[2:].strip()
            return out
    return ""


def catalogue() -> dict[str, tuple[str, str]]:
    found: dict[str, tuple[str, str]] = {}
    for path in sorted((ROOT / "runtime").rglob("*")):
        if path.suffix not in {".c", ".cpp", ".h"}:
            continue
        lines = path.read_text(errors="replace").splitlines()
        for i, line in enumerate(lines):
            for name in NAME.findall(line) + [f"MKW_{k}" for k in KNOB.findall(line)]:
                where = f"{path.relative_to(ROOT)}:{i + 1}"
                text = comment(lines, i) or doc_mention(lines, name)
                if name not in found or (text and not found[name][1]):
                    found[name] = (where, text)
    return found


def main() -> None:
    pat = re.compile(sys.argv[1]) if len(sys.argv) > 1 else None
    for name, (where, text) in sorted(catalogue().items()):
        if pat and not pat.search(name):
            continue
        print(f"{name}\t{where}\t{text[:220]}")


if __name__ == "__main__":
    main()
