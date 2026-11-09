"""A diagnostic fragment shader: every texture sample of a translated FS shown in its own vertical band of the screen.

Why: a draw that comes out black or wrong (the foliage of the race, 01/11/2026) has to be split into what each texture
gives it before blaming the translator, the descriptor or the data. One run with this FS shows them all at once:
band k (of 1280 / N px) is the RGB of the k-th texture() call, the last band is the shader's own colour. The SPIR-V
replaces build/spirv/<key>.spv (a copy of the original is kept as <key>.spv.orig; --restore puts it back).

Usage: uv run tools/shaders/fs_bands.py KEY            (writes the diagnostic SPIR-V in place)
       uv run tools/shaders/fs_bands.py KEY --restore
       uv run tools/shaders/fs_bands.py --ids KEY...  (each FS a flat colour of its own: which program draws a pixel;
                                                       the colours go to scratchpad/fs_ids.tsv)
"""

import argparse
import re
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TEX = re.compile(r"\{ vec4 tv = (texture|textureLod|textureGrad|texelFetch|textureGather)\(")
OUT = "out_color0[0] = uintBitsToFloat(r0);"


def bands(src: str) -> str:
    lines = src.split("\n")
    out, n = [], 0
    for ln in lines:
        if TEX.search(ln):
            ln = ln[: ln.rindex("}")] + f"dbg[{n}] = tv; }}"
            n += 1
        out.append(ln)
    if not n:
        raise SystemExit("fs_bands: no texture sample in this shader")
    body = "\n".join(out)
    body = body.replace("void main() {", f"void main() {{\n    vec4 dbg[{n}];\n    for (int k = 0; k < {n}; k++) dbg[k] = vec4(0.0);", 1)
    pick = (f"{{ int b = clamp(int(gl_FragCoord.x) * {n + 1} / 1280, 0, {n}); "
            f"if (b < {n}) {{ out_color0 = vec4(dbg[b].rgb, 1.0); return; }} }} ")
    body = body.replace(OUT, pick + OUT)
    return body


def flat(src: str, rgb: tuple[int, int, int]) -> str:
    c = ", ".join(f"{v / 255:.4f}" for v in rgb)
    return src.replace(OUT, f"{{ out_color0 = vec4({c}, 1.0); return; }} " + OUT)


def ids(keys: list[str]) -> None:
    """Colours on a 7x7x7 grid (343 shaders) from 30 to 240, so no id is black or white."""
    rows = []
    for n, key in enumerate(keys):
        rgb = tuple(30 + 35 * ((n // d) % 7) for d in (1, 7, 49))
        compile_to(key, flat((ROOT / "build/spirv" / f"{key}.glsl").read_text(), rgb))
        rows.append(f"{key}\t{rgb[0]},{rgb[1]},{rgb[2]}")
    (ROOT / "scratchpad/fs_ids.tsv").write_text("\n".join(rows) + "\n")
    print(f"{len(keys)} shaders; colours in scratchpad/fs_ids.tsv")


def compile_to(key: str, src: str) -> None:
    base = ROOT / "build/spirv" / key
    spv, orig = base.with_suffix(".spv"), Path(f"{base}.spv.orig")
    if not orig.exists():
        shutil.copy(spv, orig)
    gl = ROOT / "scratchpad" / f"{key}.diag.frag"
    gl.write_text(src)
    subprocess.run(["glslc", "--target-env=vulkan1.3", "-fshader-stage=frag", "-O", str(gl), "-o", str(spv)], check=True)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("key", nargs="+")
    ap.add_argument("--restore", action="store_true")
    ap.add_argument("--ids", action="store_true")
    a = ap.parse_args()
    if a.ids:
        ids(a.key)
        return
    if a.restore:
        for k in a.key:
            orig = ROOT / "build/spirv" / f"{k}.spv.orig"
            shutil.move(orig, orig.with_suffix(""))
            print(f"restored {k}")
        return
    for k in a.key:
        compile_to(k, bands((ROOT / "build/spirv" / f"{k}.glsl").read_text()))
        print(f"wrote build/spirv/{k}.spv (original in {k}.spv.orig)")


if __name__ == "__main__":
    main()
