"""Thread snapshot of a running build/mkw: every host thread's guest call chain, with module+offset and names.

Why: a hang shows nothing in the log. gdb's backtrace of the process lists lifted functions as f_<guest address>;
mapped through build/bindb.sqlite they read as the game's own call chain, plus the host frame the thread sits in (a
futex wait, a Vulkan call...). One short block per thread instead of a raw gdb dump.

Usage: uv run tools/run/stacks.py [--pid PID] [--depth N] [--gdb-out FILE]
Without --gdb-out it attaches gdb to PID (default: the running mkw) for one batch backtrace; the process keeps running.
"""

import argparse
import re
import sqlite3
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FRAME = re.compile(r"^#\d+\s+(?:0x[0-9a-f]+ in )?(\S+)")
GUEST = re.compile(r"^f_([0-9a-f]+)$")


def gdb_dump(pid: int, depth: int) -> str:
    cmd = ["gdb", "-p", str(pid), "-batch", "-ex", "set pagination off", "-ex", f"thread apply all bt {depth}"]
    return subprocess.run(cmd, capture_output=True, text=True, check=False).stdout


class Names:
    def __init__(self) -> None:
        self.db = sqlite3.connect(ROOT / "build/bindb.sqlite")
        self.mods = self.db.execute("SELECT name, base, size FROM module").fetchall()

    def __call__(self, a: int) -> str:
        rel = next((f"{n}+{a - b:#x}" for n, b, s in self.mods if b <= a < b + s), f"{a:#x}")
        row = self.db.execute("SELECT name FROM func WHERE addr=?", (a,)).fetchone()
        return f"{rel} {row[0]}" if row and row[0] else rel


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--pid", type=int)
    ap.add_argument("--depth", type=int, default=24)
    ap.add_argument("--gdb-out", type=Path, help="parse a saved gdb 'thread apply all bt' output instead")
    ap.add_argument("--host", action="store_true", help="also list threads with no guest frames (back end, decoders)")
    ap.add_argument("--save", type=Path, help="keep the raw gdb output here")
    args = ap.parse_args()
    if args.gdb_out:
        text = args.gdb_out.read_text(errors="replace")
    else:
        pid = args.pid or int(
            subprocess.run(["pgrep", "-x", "mkw"], capture_output=True, text=True, check=True).stdout.split()[0]
        )
        text = gdb_dump(pid, args.depth)
        if args.save:
            args.save.write_text(text)
    names = Names()
    threads = re.split(r"\n(?=Thread \d+ )", text)
    groups: dict[tuple, list[str]] = {}
    for block in threads:
        if not block.startswith("Thread "):
            continue
        host, guest = None, []
        for line in block.splitlines()[1:]:
            m = FRAME.match(line.strip())
            if not m:
                continue
            g = GUEST.match(m.group(1))
            if g:
                guest.append(names(int(g.group(1), 16)))
            elif host is None:
                host = m.group(1)
        if guest:
            groups.setdefault((host, *guest[:8]), []).append(block.split()[1])
        elif args.host:  # host-only threads (back end, audio, decoders): their first named frames
            frames = [m.group(1) for line in block.splitlines()[1:] if (m := FRAME.match(line.strip()))]
            named = [f for f in frames if f not in ("??", "syscall")][:4]
            groups.setdefault(("host", *named), []).append(block.split()[1])
    shown = sum(len(v) for v in groups.values())
    # Threads with the same stack are one line: idle workers waiting on the same queue do not bury the rest.
    for key, ids in sorted(groups.items(), key=lambda kv: len(kv[1])):
        print(f"{len(ids)} thread(s) {','.join(ids[:6])}{'...' if len(ids) > 6 else ''}: host {key[0]}")
        for g in key[1:]:
            print(f"    {g}")
    print(f"{shown} threads running guest code", file=sys.stderr)


if __name__ == "__main__":
    main()
