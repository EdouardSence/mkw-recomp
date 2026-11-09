"""The waits for the runtime's locks in a run with MKW_LOCK_PROF=1: per lock and per caller, in ms per second of race.

Why: comparing the kernel lock G before and after a change needs the race's windows
of the "perf: lock waits" lines added up and named; by hand, the log's addresses were read wrong once (06/11/2026:
sym.py took the absolute addresses for offsets). Lock and caller are named from build/mkw's symbols (nm, addr2line).
Only the windows after the race starts (MKW_SEQ_UNTIL's "watch off" line) count. The holders' part: how long the
caller that took a lock kept it while some thread slept waiting for it (which section to shorten).

Usage: uv run tools/run/locks.py build/run/NAME.log [...] [--top 12]   (the build/mkw of that run)
       uv run tools/run/locks.py build/run/wNAME.log --exe build/dist/windows/mkw.exe   (a Windows run: its exe)
"""

import argparse
import re
import subprocess
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXE = ROOT / "build/mkw"
BASE = 0x200000  # first PT_LOAD of build/mkw, as sym.py; a PE's ImageBase (main)
ENTRY = re.compile(r"exe\+(0x[0-9a-f]+) from exe\+(0x[0-9a-f]+) ([\d.]+) ms \((\d+)\)")
WINDOW = re.compile(r"perf: frame \d+: interval avg ([\d.]+) ms")


def image_base(exe: Path) -> int:
    """The ImageBase of a PE (the log's offsets are from the loaded module), or the ELF's BASE."""
    if exe.read_bytes()[:2] != b"MZ":
        return 0x200000
    out = subprocess.run(["objdump", "-p", str(exe)], capture_output=True, text=True, check=True).stdout
    return int(re.search(r"ImageBase\s+([0-9a-fA-F]+)", out).group(1), 16)


def symbols() -> list[tuple[int, str]]:
    out = subprocess.run(["nm", "-n", str(EXE)], capture_output=True, text=True, check=True).stdout
    rows = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "bBdD":
            rows.append((int(parts[0], 16), parts[2]))
    return rows


def lock_name(addr: int, syms: list[tuple[int, str]]) -> str:
    best = max((s for s in syms if s[0] <= addr), default=(0, "?"))
    return best[1] if addr - best[0] < 4096 else hex(addr)


def callers(addrs: list[int]) -> dict[int, str]:
    if not addrs:
        return {}
    cmd = ["addr2line", "-f", "-s", "-e", str(EXE), *(hex(a) for a in addrs)]
    lines = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout.splitlines()
    return {a: f"{lines[2 * i]} {lines[2 * i + 1]}" for i, a in enumerate(addrs)}


def by_caller(title: str, waits: Counter, counts: Counter, seconds: float, syms, top: int) -> list[str]:
    names = callers(sorted({ra for _, ra in waits}))
    out = [title]
    for (lk, ra), ms in waits.most_common(top):
        out.append(f"  {ms / seconds:7.1f}  {lock_name(lk, syms):<14} {counts[(lk, ra)] / seconds:8.0f}/s  {names[ra]}")
    return out


def summary(path: Path, syms: list[tuple[int, str]], top: int) -> list[str]:
    waits: Counter = Counter()
    counts: Counter = Counter()
    holds: Counter = Counter()
    hcounts: Counter = Counter()
    seconds, racing = 0.0, False
    for line in path.read_text(errors="replace").splitlines():
        if "seq: watch off" in line:
            racing = True
        if not racing:
            continue
        w = WINDOW.search(line)
        if w:
            seconds += 60 * float(w.group(1)) / 1000
        held = "lock holds a thread slept on" in line
        if held or "lock waits since the last line" in line:
            ms_by, n_by = (holds, hcounts) if held else (waits, counts)
            for lk, ra, ms, n in ENTRY.findall(line):
                key = (BASE + int(lk, 16), BASE + int(ra, 16))
                ms_by[key] += float(ms)
                n_by[key] += int(n)
    if not seconds:
        return [f"== {path}: no race windows with lock waits (MKW_LOCK_PROF=1 MKW_TRACE=perf)"]
    by_lock: Counter = Counter()
    for (lk, _), ms in waits.items():
        by_lock[lock_name(lk, syms)] += ms
    out = [f"== {path}: {seconds:.1f} s of race; lock waits in ms per s (the ten longest per window only)"]
    out += [f"  {ms / seconds:7.1f}  {name}" for name, ms in by_lock.most_common(6)]
    out += by_caller("  by caller:", waits, counts, seconds, syms, top)
    if holds:
        out += by_caller("  holders a thread slept on, by the caller that took it:", holds, hcounts, seconds, syms, top)
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("logs", nargs="+", type=Path)
    ap.add_argument("--top", type=int, default=12)
    global EXE, BASE
    ap.add_argument("--exe", type=Path, default=EXE, help="the binary of those runs (default build/mkw)")
    args = ap.parse_args()
    EXE, BASE = args.exe, image_base(args.exe)
    syms = symbols()
    for p in args.logs:
        print("\n".join(summary(p, syms, args.top)))


if __name__ == "__main__":
    main()
