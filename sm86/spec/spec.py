"""Build the SM86 instruction spec the shader decoder reads (instructions.json) on this machine.

Why: the spec is NVIDIA's own description of the encoding, which nvdisasm only holds in memory: DocumentSASS catches it
by intercepting memcpy (intercept.c, funnel.py) and nvsass-disassembler parses it (parse_sidl.py, sidl_*.py), all MIT
and copied here. Being NVIDIA's text it is never versioned nor shipped: the player's setup (tools/prepare/prepare.py)
and a source checkout build it from CUDA 12.8's redistributable nvdisasm and ptxas, fetched from NVIDIA and pinned by
sha256. CUDA 12.8 is the last release whose nvdisasm still yields the text. nvdisasm needs any sm_86 cubin to run: one
is assembled from a minimal PTX with ptxas.

Linux only for now: the interception is an LD_PRELOAD library.

Usage: python sm86/spec/spec.py [--out build/sm86] [--cache scratchpad/nvidia] [--cc "zig cc"]
       (~1 min; downloads 84 MB the first time)
"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
REDIST = "https://developer.download.nvidia.com/compute/cuda/redist"
PINS = {  # tool: (package path under REDIST, sha256)
    "nvdisasm": ("cuda_nvdisasm/linux-x86_64/cuda_nvdisasm-linux-x86_64-12.8.90-archive.tar.xz",
                 "8ac63f4079c47707ec958af3a97ceeb18194e16827a2766058f5872f6c3412ae"),
    "ptxas": ("cuda_nvcc/linux-x86_64/cuda_nvcc-linux-x86_64-12.8.93-archive.tar.xz",
              "9961b3484b6b71314063709a4f9529654f96782ad39e72bf1e00f070db8210d3"),
}  # fmt: skip
TRIES = 5
PTX = """.version 8.7
.target sm_86
.address_size 64
.visible .entry k(.param .u64 p)
{
  .reg .b64 %rd<2>;
  .reg .b32 %r<2>;
  ld.param.u64 %rd1, [p];
  mov.u32 %r1, %tid.x;
  st.global.u32 [%rd1], %r1;
  ret;
}
"""


def fetch(rel: str, want: str, cache: Path) -> Path:
    dst = cache / rel.rsplit("/", 1)[1]
    if not dst.is_file():
        part = dst.with_name(dst.name + ".part")
        with urllib.request.urlopen(f"{REDIST}/{rel}") as r, open(part, "wb") as f:
            shutil.copyfileobj(r, f)
        part.rename(dst)
    got = hashlib.sha256(dst.read_bytes()).hexdigest()
    if got != want:
        dst.unlink()
        sys.exit(f"{dst.name}: sha256 {got}, expected {want}")
    return dst


def tool(name: str, cache: Path) -> Path:
    """bin/<name> of its pinned package, unpacked once into cache/bin."""
    exe = cache / "bin" / name
    if exe.is_file():
        return exe
    rel, want = PINS[name]
    with tarfile.open(fetch(rel, want, cache)) as t:
        member = next(m for m in t.getmembers() if m.name.endswith(f"/bin/{name}"))
        exe.parent.mkdir(parents=True, exist_ok=True)
        with t.extractfile(member) as src, open(exe, "wb") as dst:
            shutil.copyfileobj(src, dst)
    exe.chmod(0o755)
    return exe


def strings(data: bytes) -> str:
    """`strings -n 1`: every run of printable ASCII (and tab), one per line."""
    return "\n".join(m.decode("ascii") for m in re.findall(rb"[\t\x20-\x7e]+", data)) + "\n"


def no_aslr() -> None:
    """nvdisasm with fixed addresses (setarch -R): the captured buffers hold pointers, and with random ones the cut text
    came out garbled in 5 of ~17 tries, against 0 of 6 without (09/11/2026)."""
    import ctypes

    ctypes.CDLL(None).personality(0x0040000)  # ADDR_NO_RANDOMIZE


def capture(nvdisasm: Path, so: Path, work: Path, out: Path) -> str:
    """One interception of nvdisasm, cut and parsed into out/; the parser's complaint, or '' when the spec is good."""
    env = dict(os.environ, LD_PRELOAD=str(so), OMP_NUM_THREADS="1", OMP_THREAD_LIMIT="1")
    dump = subprocess.run([str(nvdisasm), str(work / "sm_86.cubin")], env=env, capture_output=True, check=True,
                          preexec_fn=no_aslr)  # fmt: skip
    (work / "sm_86_intercept.txt").write_text(strings(dump.stdout), encoding="ascii")
    p = subprocess.run([sys.executable, str(HERE / "funnel.py"), str(work / "sm_86_intercept.txt")],
                       capture_output=True, text=True)  # fmt: skip
    if p.returncode:
        return f"funnel.py: {p.stderr[-2000:]}"
    out.mkdir(parents=True, exist_ok=True)
    for name in ("sm_86_instructions.txt", "sm_86_latencies.txt"):
        shutil.move(work / name, out / name)
    # Exit 1 means "something was reported", not "no spec": the one known report (a missing separator in
    # f2f_f32_upconvert__RCR_RCR) is repaired by the parser itself, so anything beyond it fails.
    p = subprocess.run([sys.executable, str(HERE / "parse_sidl.py"), str(out / "sm_86_instructions.txt"), "-o", str(out)],
                       capture_output=True, text=True)  # fmt: skip
    (out / "parse.log").write_text(p.stdout + p.stderr)
    if p.returncode > 1 or not re.search(r"^unparsed / suspect items: [01]$", p.stdout, re.M):
        return f"parse_sidl.py: {p.stdout[-2000:]}{p.stderr[-2000:]}"
    return ""


def build(out: Path, cache: Path, cc: list[str]) -> None:
    if os.name == "nt":
        sys.exit("spec.py: generating the SM86 spec on Windows is not supported yet")
    cache.mkdir(parents=True, exist_ok=True)
    nvdisasm, ptxas = tool("nvdisasm", cache), tool("ptxas", cache)
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        (work / "k.ptx").write_text(PTX)
        subprocess.run([str(ptxas), "-arch=sm_86", str(work / "k.ptx"), "-o", str(work / "sm_86.cubin")], check=True)
        so = work / "intercept.so"
        # -nostdlib and libc after the object: zig cc otherwise links its own memcpy and hides intercept.c's; -O2 and
        # no sanitizer: zig's debug default calls UBSan handlers nvdisasm does not have.
        subprocess.run([*cc, "-O2", "-fno-sanitize=undefined", "-fPIC", "-shared", "-nostdlib", "-o", str(so),
                        str(HERE / "intercept.c"), "-lc", "-ldl"], check=True)  # fmt: skip
        # A garbled capture is still possible (see no_aslr) and the parser has caught every one: a few tries, ~3 s each.
        for attempt in range(1, TRIES + 1):
            err = capture(nvdisasm, so, work, out)
            if not err:
                print(f"wrote {out / 'instructions.json'}")
                return
            print(f"capture {attempt} of {TRIES} rejected: {err.splitlines()[0]}", file=sys.stderr)
    sys.exit(err)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", type=Path, default=ROOT / "build/sm86")
    ap.add_argument("--cache", type=Path, default=ROOT / "scratchpad/nvidia", help="downloads and unpacked tools")
    ap.add_argument("--cc", default="cc", help="C compiler command for the interception library")
    args = ap.parse_args()
    build(args.out, args.cache, args.cc.split())


if __name__ == "__main__":
    main()
