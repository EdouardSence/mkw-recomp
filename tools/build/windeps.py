"""Fetch what the Windows build needs into build/win: llvm-mingw (clang + lld for x86_64-w64-mingw32, UCRT) and the
headers, import libraries and DLLs of SDL3, Opus, libvpx, zstd, Vulkan, VMA and xxHash.

Why: the Windows exe is cross-compiled from Linux (tools/build/build.py --target windows) the way totk-pc-recomp builds
its installer with --toolchain: llvm-mingw plus the MSYS2 UCRT64 packages (pins taken from its tools/make_release.py,
MIT), and the upstream SDL3 mingw development package. Every download is checked against its sha256; a mismatch stops.

Layout:
    build/win/llvm-mingw/   the toolchain (bin/x86_64-w64-mingw32-clang)
    build/win/deps/         include/, lib/ (import and static libraries), bin/ (the DLLs mkw.exe loads)

Usage: uv run tools/build/windeps.py [--dry-run]   (downloads cached in build/win/dl; ~250 MB unpacked)
"""

import argparse
import hashlib
import shutil
import sys
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WIN = ROOT / "build/win"
DL = WIN / "dl"
MSYS2 = "https://repo.msys2.org/mingw/ucrt64/"
LLVM_VERSION = "20261020"  # clang 23, as totk-pc-recomp's release
SDL_VERSION = "3.4.18"
# name: (url, sha256)
PINS = {
    "llvm-mingw": (
        (
            f"https://github.com/mstorsjo/llvm-mingw/releases/download/{LLVM_VERSION}/"
            f"llvm-mingw-{LLVM_VERSION}-ucrt-ubuntu-22.04-x86_64.tar.xz"
        ),
        "55064b60b991a1e1e8090b8c2f84e82b22cfe5903d12c159d65064cf5e8ae335",
    ),
    "sdl3": (
        f"https://github.com/libsdl-org/SDL/releases/download/release-{SDL_VERSION}/SDL3-devel-{SDL_VERSION}-mingw.tar.gz",
        "049b9dd711e3fb3f8d84feb998dfec54a75e17cf539c4a80ff4d058d839771cf",
    ),
    "opus": (
        MSYS2 + "mingw-w64-ucrt-x86_64-opus-1.6.1-2-any.pkg.tar.zst",
        "bb114bcf26225f50f874497ebfd6b8224fff2a20bda3f8ee45cd07ce5f02657b",
    ),
    "libvpx": (
        MSYS2 + "mingw-w64-ucrt-x86_64-libvpx-1.17.0-1-any.pkg.tar.zst",
        "8ff6919dbb88ea26ce28d22fd81d6f0d6e200cf56a4dd81ffc2d33124fe98024",
    ),
    "vulkan-headers": (
        MSYS2 + "mingw-w64-ucrt-x86_64-vulkan-headers-1~1.4.363.0-1-any.pkg.tar.zst",
        "be602b2657cace531316e78b1eda4087e8f7f84ebb877f08fba198a2db06406a",
    ),
    "vulkan-loader": (
        MSYS2 + "mingw-w64-ucrt-x86_64-vulkan-loader-1~1.4.363.0-1-any.pkg.tar.zst",
        "065076fe13d39ab97eea4740596426d5ae741b5044e48e1c9495d04835c92dbe",
    ),
    "vma": (
        MSYS2 + "mingw-w64-ucrt-x86_64-vulkan-memory-allocator-3.4.0-1-any.pkg.tar.zst",
        "44c81162337915afecda3185f66fd98268a54454580b2717080c9dfef57fe781",
    ),
    "xxhash": (  # header only (XXH_INLINE_ALL in runtime/gfx/vk/tex/vk_texpre.c); 0.8.4 as Arch's
        MSYS2 + "mingw-w64-ucrt-x86_64-xxhash-0.8.4-1-any.pkg.tar.zst",
        "a22d39e85edd6f0b4c7fe34733d28f17d49715f6c8dea67c142161c02bfbbde2",
    ),
    "zstd": (
        MSYS2 + "mingw-w64-ucrt-x86_64-zstd-1.5.7-2-any.pkg.tar.zst",
        "dbdb8427280046a2b41697780aa4c52983b708082b0da4755951dc3bea96ca89",
    ),
}
# vulkan-1.dll is the system's (the GPU driver installs it): only its import library is kept.
SKIP_DLLS = {"vulkan-1.dll"}


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def fetch(name: str) -> Path:
    url, want = PINS[name]
    path = DL / url.rsplit("/", 1)[1].replace("%7E", "~")
    if not path.is_file():
        print(f"get {url}")
        part = path.with_suffix(path.suffix + ".part")
        with urllib.request.urlopen(url) as r, part.open("wb") as f:
            shutil.copyfileobj(r, f)
        part.rename(path)
    got = sha256(path)
    if got != want:
        sys.exit(f"{path.name}: sha256 {got}, expected {want}")
    return path


def unpack_llvm(archive: Path) -> None:
    dst = WIN / "llvm-mingw"
    stamp = dst / ".version"
    if stamp.is_file() and stamp.read_text() == LLVM_VERSION:
        return
    shutil.rmtree(dst, ignore_errors=True)
    tmp = WIN / "llvm-mingw.tmp"
    shutil.rmtree(tmp, ignore_errors=True)
    with tarfile.open(archive) as t:
        t.extractall(tmp, filter="tar")
    (top,) = tmp.iterdir()
    top.rename(dst)
    tmp.rmdir()
    stamp.write_text(LLVM_VERSION)


def unpack_deps(archives: dict[str, Path]) -> None:
    """Each package's include/, lib/ and bin/*.dll into build/win/deps (MSYS2: ucrt64/..., SDL: x86_64-w64-mingw32/...)."""
    dst = WIN / "deps"
    shutil.rmtree(dst, ignore_errors=True)
    for name, archive in archives.items():
        prefix = "x86_64-w64-mingw32/" if name == "sdl3" else "ucrt64/"
        with tarfile.open(archive) as t:
            for m in t.getmembers():
                rel = m.name.split(prefix, 1)[1] if prefix in m.name else ""
                top = rel.split("/", 1)[0]
                if not m.isfile() or top not in ("include", "lib", "bin"):
                    continue
                if top == "bin" and (not rel.endswith(".dll") or Path(rel).name in SKIP_DLLS):
                    continue
                out = dst / rel
                out.parent.mkdir(parents=True, exist_ok=True)
                with t.extractfile(m) as src, out.open("wb") as f:
                    shutil.copyfileobj(src, f)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dry-run", action="store_true", help="list the pinned downloads and stop")
    args = ap.parse_args()
    if args.dry_run:
        for name, (url, want) in PINS.items():
            print(f"{name}: {url} {want}")
        return
    DL.mkdir(parents=True, exist_ok=True)
    paths = {name: fetch(name) for name in PINS}
    unpack_llvm(paths.pop("llvm-mingw"))
    unpack_deps(paths)
    dlls = sorted(p.name for p in (WIN / "deps/bin").glob("*.dll"))
    print(f"build/win: llvm-mingw {LLVM_VERSION}, deps {', '.join(PINS)[len('llvm-mingw, ') :]}; DLLs {' '.join(dlls)}")


if __name__ == "__main__":
    main()
