"""Count the jumps to themselves in build/mkw against the guest's own `b .` loops in the generated C, per function.

Why: a host `jmp` to itself hangs the thread that reaches it. Most come straight from guest code that loops on
itself (`b .` after a call that never returns, or on a path that must not happen); the ones a function has beyond its
guest loops are clang's (a spin wait whose load it hoisted, undefined behaviour) and need a look.

Usage: uv run tools/cpu/self_loops.py [--bin build/mkw] [--gen build/generated]   (~2 min: objdump of the binary)
Prints both totals and every function with more host self-jumps than guest self-loops.
"""

import argparse
import re
import subprocess
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FUNC = re.compile(r"^[0-9a-f]+ <(.+)>:$")
JMP = re.compile(r"^\s+([0-9a-f]+):\s+jmp\s+([0-9a-f]+)\b")
C_FUNC = re.compile(r"^void (f_[0-9a-f]+)\(Cpu \*c\) \{")
C_LABEL = re.compile(r"^L_([0-9a-f]+):;")


def host_counts(binary: Path) -> Counter:
    out, fn = Counter(), None
    dis = subprocess.Popen(["objdump", "-d", "--no-show-raw-insn", str(binary)], stdout=subprocess.PIPE, text=True)
    for line in dis.stdout:
        if m := FUNC.match(line):
            fn = re.sub(r"[+.].*", "", m.group(1))
        elif (m := JMP.match(line)) and m.group(1) == m.group(2):
            out[fn] += 1
    dis.wait()
    return out


def guest_counts(gen: Path) -> Counter:
    """A label followed by an unconditional goto to itself: the lifted `b .`, or a body-less loop of the guest."""
    out, fn = Counter(), None
    for src in sorted(gen.glob("*/*.c")):
        prev = None
        for line in src.read_text().splitlines():
            if m := C_FUNC.match(line):
                fn = m.group(1)
            if prev and "if (" not in line and re.search(rf"goto L_{prev};\s*$", line):
                out[fn] += 1
            m = C_LABEL.match(line)
            prev = m.group(1) if m else None
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--bin", type=Path, default=ROOT / "build/mkw")
    ap.add_argument("--gen", type=Path, default=ROOT / "build/generated")
    args = ap.parse_args()
    host, guest = host_counts(args.bin), guest_counts(args.gen)
    print(f"host jumps to themselves {sum(host.values())}, guest self-loops in the C {sum(guest.values())}")
    extra = {f: host[f] - guest[f] for f in host if host[f] > guest[f]}
    print(f"functions with more host self-jumps than guest self-loops: {len(extra)} ({sum(extra.values())} jumps)")
    for f, n in sorted(extra.items()):
        print(f"  {f} +{n}")


if __name__ == "__main__":
    main()
