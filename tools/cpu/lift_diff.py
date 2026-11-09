"""Differential test of the lifter's newer instruction classes against Unicorn (QEMU's AArch64, CPU model "max").

Why: the classes Switch 1 games never used (LSE atomics, CAS/CASP, LDAPR, XPAC, RETAA/RETAB) were added to
recomp/lifter.py from the specification; this runs each one, with random registers, operands and memory, both as the
lifter's C and in Unicorn, and compares every general register and the memory around the address. LDSMAX/LDSMIN memory is
compared with the Arm pseudocode instead (Unicorn compares unsigned). PAC*/AUT* are not
compared: on the host they are the identity on purpose (pointers are never signed), while Unicorn signs.

Usage: uv run --with unicorn tools/cpu/lift_diff.py [--n 2000] [--seed 1]
Prints the cases per class and every mismatch (word, disassembly fields, register or byte that differs).
"""

import argparse
import ctypes
import random
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import unicorn.arm64_const as uca
from unicorn import UC_ARCH_ARM64, UC_MODE_ARM, Uc, UcError
from unicorn.arm64_const import UC_ARM64_REG_SP, UC_CPU_ARM64_MAX

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "recomp"))

from lifter import Lifter

MEM = 0x10000  # bytes of guest memory around the address, same contents on both sides
CODE = 0x1000  # Unicorn: where the instruction sits
XREG = [getattr(uca, f"UC_ARM64_REG_X{i}") for i in range(31)]  # X29 and X30 are not after X28


def lse(rng: random.Random) -> tuple[str, int, int]:
    """LD<op>{A}{L}{B,H} / SWP: size, A, R, Rs, o3, opc, Rn, Rt."""
    size, a, r, o3 = rng.randrange(4), rng.randrange(2), rng.randrange(2), rng.randrange(2)
    opc = 0 if o3 else rng.randrange(8)
    rn, rs, rt = regs(rng, 3)
    w = size << 30 | 0b111000 << 24 | a << 23 | r << 22 | 1 << 21 | rs << 16 | o3 << 15 | opc << 12 | rn << 5 | rt
    return ("swp" if o3 else ("add", "clr", "eor", "set", "smax", "smin", "umax", "umin")[opc]), w, 1 << size


def ldapr(rng: random.Random) -> tuple[str, int, int]:
    size = rng.choice((0, 1, 2, 3))
    rn, rt = regs(rng, 2)
    return (
        "ldapr",
        size << 30 | 0b111000 << 24 | 1 << 23 | 1 << 21 | 31 << 16 | 1 << 15 | 4 << 12 | rn << 5 | rt,
        1 << size,
    )


def cas(rng: random.Random) -> tuple[str, int, int]:
    size, l_, o0 = rng.randrange(4), rng.randrange(2), rng.randrange(2)
    rn, rs, rt = regs(rng, 3)
    w = size << 30 | 0b0010001 << 23 | l_ << 22 | 1 << 21 | rs << 16 | o0 << 15 | 31 << 10 | rn << 5 | rt
    return "cas", w, 1 << size


def casp(rng: random.Random) -> tuple[str, int, int]:
    sz, l_, o0 = rng.randrange(2), rng.randrange(2), rng.randrange(2)
    while True:
        rn = rng.randrange(31)
        rs, rt = rng.randrange(0, 30, 2), rng.randrange(0, 30, 2)
        if len({rn, rs, rs + 1, rt, rt + 1}) == 5:
            break
    w = sz << 30 | 0b0010000 << 23 | l_ << 22 | 1 << 21 | rs << 16 | o0 << 15 | 31 << 10 | rn << 5 | rt
    return "casp", w, 16 if sz else 8


def xpac(rng: random.Random) -> tuple[str, int, int]:
    (rd,) = regs(rng, 1)
    return "xpac", rng.choice((0xDAC143E0, 0xDAC147E0)) | rd, 0


def regs(rng: random.Random, n: int) -> list[int]:
    """Distinct registers: the first is the address base (never XZR/SP), the others may be XZR."""
    out = [rng.randrange(31)]
    while len(out) < n:
        r = rng.randrange(32)
        if r not in out:
            out.append(r)
    return out


CLASSES = (lse, ldapr, cas, casp, xpac)

HARNESS = """
#include "cpu.h"
_Alignas(4096) uint8_t lift_mem[%d];
size_t cpu_size(void) { return sizeof(Cpu); }
%s
void (*const cases[])(Cpu *) = { %s };
"""


def build(cases: list[tuple[str, int, int]], tmp: Path) -> ctypes.CDLL:
    lift = Lifter(0, simd="c")
    fns = []
    for k, (_, w, _) in enumerate(cases):
        ins = lift.lift(w, CODE)
        if ins.kind != "op":
            sys.exit(f"{w:#010x}: lifted as {ins.kind}, not an op")
        fns.append(f"static void t{k}(Cpu *c) {{ {ins.code} }}")
    src = tmp / "h.c"
    src.write_text(HARNESS % (MEM, "\n".join(fns), ", ".join(f"t{k}" for k in range(len(cases)))))
    lib = tmp / "h.so"
    subprocess.run(
        ["clang", "-std=gnu11", "-O2", "-shared", "-fPIC", "-march=x86-64-v3", f"-I{ROOT / 'runtime/cpu'}",
         f"-I{ROOT / 'runtime'}", f"-I{ROOT / 'runtime/core'}", "-o", str(lib), str(src)],
        check=True,
    )  # fmt: skip
    return ctypes.CDLL(str(lib))


def operands(rng: random.Random, align: int) -> tuple[list[int], bytes, int]:
    """Random registers, memory, and an address aligned to the access in the middle of it."""
    x = [rng.getrandbits(64) for _ in range(31)]
    # Small values too: the compare of CAS and the min/max signs need equal and near values to be exercised.
    for k in range(31):
        if rng.random() < 0.3:
            x[k] = rng.choice((0, 1, 0x7F, 0x80, 0xFFFF, 0x80000000, 0xFFFFFFFF, (1 << 63), (1 << 64) - 1))
    mem = bytes(rng.getrandbits(8) for _ in range(MEM))
    off = MEM // 2 + rng.randrange(64) * max(align, 1)
    return x, mem, off


def run(lib: ctypes.CDLL, k: int, w: int, x: list[int], mem: bytes, base_reg: int, off: int) -> list[str]:
    host_mem = ctypes.addressof(ctypes.c_uint8.in_dll(lib, "lift_mem"))
    ctypes.memmove(host_mem, mem, MEM)
    regs = list(x)
    regs[base_reg] = host_mem + off
    cpu = (ctypes.c_uint64 * (lib.cpu_size() // 8))()
    for i in range(31):
        cpu[i] = regs[i]
    cpu[32] = host_mem + MEM - 256  # sp
    fn = ctypes.cast(
        ctypes.cast(lib.cases, ctypes.POINTER(ctypes.c_void_p))[k], ctypes.CFUNCTYPE(None, ctypes.c_void_p)
    )
    fn(ctypes.addressof(cpu))
    got_x, got_mem = [cpu[i] for i in range(31)], ctypes.string_at(host_mem, MEM)

    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.ctl_set_cpu_model(UC_CPU_ARM64_MAX)
    uc.mem_map(CODE, 0x1000)
    uc.mem_write(CODE, struct.pack("<I", w))
    uc.mem_map(host_mem, MEM)
    uc.mem_write(host_mem, mem)
    for i in range(31):
        uc.reg_write(XREG[i], regs[i])
    uc.reg_write(UC_ARM64_REG_SP, host_mem + MEM - 256)
    try:
        uc.emu_start(CODE, CODE + 4)
    except UcError as e:
        return [f"Unicorn: {e} (address {regs[base_reg]:#x})"]
    ref_x, ref_mem = [uc.reg_read(XREG[i]) for i in range(31)], bytes(uc.mem_read(host_mem, MEM))

    if (w >> 24) & 0x3F == 0b111000 and not (w >> 15) & 1 and (w >> 12) & 7 in (4, 5):
        ref_mem = signed_minmax(w, regs, mem, off)
    bad = [f"x{i} {got_x[i]:#x} != {ref_x[i]:#x}" for i in range(31) if got_x[i] != ref_x[i]]
    bad += [f"mem+{j - off:#x} {got_mem[j]:#04x} != {ref_mem[j]:#04x}" for j in range(MEM) if got_mem[j] != ref_mem[j]]
    return bad


def signed_minmax(w: int, regs: list[int], mem: bytes, off: int) -> bytes:
    """LDSMAX/LDSMIN memory result from the Arm ARM pseudocode (signed compare at the access size). Unicorn 2 compares
    unsigned there (06/11/2026: LDSMINB of 0xbc and 0 stored 0), so its memory is not the reference for these."""
    n = 1 << (w >> 30)
    rs = (w >> 16) & 31
    v = regs[rs] if rs < 31 else 0
    old = int.from_bytes(mem[off : off + n], "little", signed=True)
    opnd = int.from_bytes((v & ((1 << 8 * n) - 1)).to_bytes(n, "little"), "little", signed=True)
    new = max(old, opnd) if (w >> 12) & 7 == 4 else min(old, opnd)
    return mem[:off] + new.to_bytes(n, "little", signed=True) + mem[off + n :]


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--n", type=int, default=2000, help="cases in all, spread over the classes")
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()
    rng = random.Random(args.seed)
    cases = [CLASSES[k % len(CLASSES)](rng) for k in range(args.n)]
    lift = Lifter(0, simd="c")
    for w in (0xD65F0BFF, 0xD65F0FFF):  # RETAA, RETAB
        if lift.lift(w, CODE).kind != "ret":
            sys.exit(f"{w:#010x} is not lifted as ret")
    with tempfile.TemporaryDirectory(dir=ROOT / "scratchpad") as tmp:
        lib = build(cases, Path(tmp))
        per, fails = {}, 0
        for k, (name, w, align) in enumerate(cases):
            x, mem, off = operands(rng, align)
            base_reg = (w >> 5) & 31
            bad = run(lib, k, w, x, mem, base_reg, off) if name != "xpac" else run(lib, k, w, x, mem, 0, off)
            per[name] = per.get(name, 0) + 1
            if bad:
                fails += 1
                if fails <= 30:
                    rs = (w >> 16) & 31
                    print(f"MISMATCH {name} {w:#010x}: {'; '.join(bad[:4])} (before: mem {mem[off : off + 8].hex()}, "
                          f"Rs x{rs} {x[rs] if rs < 31 else 0:#x})")  # fmt: skip
    print("cases: " + ", ".join(f"{n} {c}" for n, c in sorted(per.items())) + "; RETAA/RETAB lifted as ret")
    print(f"{args.n - fails} of {args.n} agree with Unicorn")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
