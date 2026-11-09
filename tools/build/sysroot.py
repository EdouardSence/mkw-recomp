"""Unpack the Steam Runtime 3 "sniper" SDK sysroot into build/sysroot, for a Linux release that starts on older systems.

Why: the runtime built against the build machine's own glibc needs that glibc or newer (Arch on 09/11/2026: 2.43, which
Ubuntu 24.04's 2.39 lacks). Built with the system clang against this sysroot (glibc 2.31, SDL3 3.4, Opus, libvpx 6)
through build.py --sysroot, it runs on SteamOS and on any distribution at least as new as Debian 11. Same approach and
the same pinned snapshot as totk-pc-recomp's tools/make_release_linux.py (MIT).

Besides the sniper files, it adds under build/sysroot/usr/local what the SDK lacks, built with that same clang: zstd
1.5.7 as a static library (the SDK has no zstd headers), the xxHash 0.8.3 header, and SDL 3.4.18 in place of the SDK's,
which links X11, Wayland, PulseAudio, ALSA and libdecor directly (the Steam Runtime brings them, a desktop not always):
ours loads them at run time, as SDL does by default, and needs only glibc. Every download pinned by sha256.

Usage: uv run tools/build/sysroot.py   (~1.2 GB download the first time, ~3 min; needs clang, lld, make, cmake, pkgconf)
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from dist import DL, fetch  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
SYSROOT = ROOT / "build/sysroot"
SNIPER = "3.0.20261021.266025"
PINS = {
    "sniper": (f"https://repo.steampowered.com/steamrt-images-sniper/snapshots/{SNIPER}/"
               "com.valvesoftware.SteamRuntime.Sdk-amd64%2Ci386-sniper-sysroot.tar.gz",
               "8f71947035f97925a311b2ed753f22efcf2f13d6f1631945b8e537f8903ce2cf"),
    "zstd": ("https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz",
             "eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3"),
    "xxhash": ("https://raw.githubusercontent.com/Cyan4973/xxHash/v0.8.3/xxhash.h",
               "17973c0dc49d9854ca26caa191f0e12f7a424b68858d9a78de3860d959d85e4b"),
    "sdl3": ("https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz",
             "9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3"),
}  # fmt: skip
# Libraries ours may need at load time: SDL3 has to load everything else itself.
SDL_NEEDED = {"libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0", "librt.so.1", "ld-linux-x86-64.so.2"}
# The parts of the SDK a C/C++ compile and link for x86_64 read, and the licences of the libraries a release bundles.
MEMBERS = ["usr/include", "usr/lib/x86_64-linux-gnu", "usr/lib/gcc/x86_64-linux-gnu", "lib/x86_64-linux-gnu", "lib64",
           "usr/share/pkgconfig", "usr/share/wayland-protocols", "usr/bin/wayland-scanner", "usr/share/doc/libopus0",
           "usr/share/doc/libvpx6"]  # fmt: skip
STAMP = SYSROOT / f".sniper-{SNIPER}"


def cc() -> str:
    return f"clang --target=x86_64-linux-gnu --sysroot={SYSROOT} -fuse-ld=lld"


def unpack_sniper() -> None:
    tarball = fetch(*PINS["sniper"])
    shutil.rmtree(SYSROOT, ignore_errors=True)
    SYSROOT.mkdir(parents=True)
    # the system tar extracts only the members, the rest of the 1.2 GB stays packed
    subprocess.run(["tar", "-xzf", str(tarball), "-C", str(SYSROOT), "--wildcards", "--no-wildcards-match-slash",
                    *MEMBERS], check=True)  # fmt: skip
    # the SDK points some links at /lib and /usr/lib, which outside its container are this machine's
    for p in SYSROOT.rglob("*"):
        if p.is_symlink() and os.readlink(p).startswith("/"):
            target = SYSROOT / os.readlink(p).lstrip("/")
            p.unlink()
            p.symlink_to(os.path.relpath(target, p.parent))


def add_deps() -> None:
    local = SYSROOT / "usr/local"
    (local / "include").mkdir(parents=True, exist_ok=True)
    (local / "lib").mkdir(parents=True, exist_ok=True)
    shutil.copy2(fetch(*PINS["xxhash"]), local / "include/xxhash.h")
    src = DL / "zstd-1.5.7"
    shutil.rmtree(src, ignore_errors=True)
    subprocess.run(["tar", "-xzf", str(fetch(*PINS["zstd"])), "-C", str(DL)], check=True)
    lib = src / "lib"
    subprocess.run(["make", "-C", str(lib), f"-j{os.cpu_count() or 4}", "libzstd.a", f"CC={cc()}", "AR=llvm-ar",
                    "ZSTD_LEGACY_SUPPORT=0", "MOREFLAGS=-O2 -fPIC"], check=True, stdout=subprocess.DEVNULL)  # fmt: skip
    shutil.copy2(lib / "libzstd.a", local / "lib/libzstd.a")
    for name in ("zstd.h", "zstd_errors.h"):
        shutil.copy2(lib / name, local / "include" / name)
    (local / "share/doc/zstd").mkdir(parents=True, exist_ok=True)
    shutil.copy2(src / "LICENSE", local / "share/doc/zstd/LICENSE")
    shutil.rmtree(src)


def build_sdl3() -> None:
    """SDL3 into usr/local, the SDK's removed so that no compile or link finds it instead."""
    sdk = SYSROOT / "usr/lib/x86_64-linux-gnu"
    for p in [*sdk.glob("libSDL3.*"), *sdk.glob("libSDL3_test.*"), sdk / "pkgconfig/sdl3.pc"]:
        p.unlink(missing_ok=True)
    shutil.rmtree(SYSROOT / "usr/include/SDL3", ignore_errors=True)
    src = DL / "SDL3-3.4.18"
    shutil.rmtree(src, ignore_errors=True)
    subprocess.run(["tar", "-xzf", str(fetch(*PINS["sdl3"])), "-C", str(DL)], check=True)
    local = SYSROOT / "usr/local"
    # The SDK's wayland-scanner: a newer one writes calls its libwayland 1.18 lacks (wl_proxy_marshal_flags), which
    # Debian 11's lacks too. It runs here with the SDK's libraries (libxml2), never its glibc (lib/, not usr/lib/).
    scanner = local / "bin/wayland-scanner"
    scanner.parent.mkdir(parents=True, exist_ok=True)
    scanner.write_text(f'#!/bin/sh\nLD_LIBRARY_PATH="{SYSROOT}/usr/lib/x86_64-linux-gnu" '
                       f'exec "{SYSROOT}/usr/bin/wayland-scanner" "$@"\n')  # fmt: skip
    scanner.chmod(0o755)
    pc = [SYSROOT / "usr/lib/x86_64-linux-gnu/pkgconfig", SYSROOT / "usr/share/pkgconfig"]
    env = dict(os.environ, PKG_CONFIG_SYSROOT_DIR=str(SYSROOT), PKG_CONFIG_PATH="",
               PKG_CONFIG_LIBDIR=os.pathsep.join(map(str, pc)))  # fmt: skip
    subprocess.run(["cmake", "-S", str(src), "-B", str(src / "build"), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                    "-DCMAKE_C_COMPILER=clang", "-DCMAKE_C_COMPILER_TARGET=x86_64-linux-gnu",
                    f"-DCMAKE_SYSROOT={SYSROOT}", "-DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld",
                    # headers and libraries only from the sysroot (this machine's liburing.h was picked up)
                    "-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY", "-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY",
                    "-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY", "-DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER",
                    "-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld", f"-DCMAKE_INSTALL_PREFIX={local}",
                    "-DCMAKE_INSTALL_LIBDIR=lib", "-DSDL_STATIC=OFF", "-DSDL_TEST_LIBRARY=OFF", "-DSDL_TESTS=OFF",
                    "-DSDL_EXAMPLES=OFF", "-DSDL_RPATH=OFF", "-DSDL_DEPS_SHARED=ON",
                    f"-DWAYLAND_SCANNER={scanner}"],
                   env=env, check=True, stdout=subprocess.DEVNULL)  # fmt: skip
    summary = subprocess.run(["cmake", "-L", "-N", str(src / "build")], capture_output=True, text=True).stdout
    print(" ".join(line for line in summary.splitlines() if line.startswith(("SDL_X11:", "SDL_WAYLAND:",
          "SDL_PULSEAUDIO:", "SDL_PIPEWIRE:", "SDL_ALSA:", "SDL_VULKAN:"))))  # fmt: skip
    subprocess.run(["cmake", "--build", str(src / "build")], check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["cmake", "--install", str(src / "build")], check=True, stdout=subprocess.DEVNULL)
    (local / "share/doc/SDL3").mkdir(parents=True, exist_ok=True)
    shutil.copy2(src / "LICENSE.txt", local / "share/doc/SDL3/LICENSE.txt")
    shutil.rmtree(src)
    lib = (local / "lib/libSDL3.so.0").resolve()
    dyn = subprocess.run(["readelf", "-d", str(lib)], capture_output=True, text=True, check=True).stdout
    extra = {line.split("[", 1)[1].rstrip("]") for line in dyn.splitlines() if "(NEEDED)" in line} - SDL_NEEDED
    if extra:
        sys.exit(f"{lib} links {' '.join(sorted(extra))}: their SDL_*_SHARED options are off")


def main() -> None:
    if len(sys.argv) > 1:
        raise SystemExit(__doc__)
    DL.mkdir(parents=True, exist_ok=True)
    if not STAMP.is_file():
        unpack_sniper()
        add_deps()
        build_sdl3()
        STAMP.write_text("")
    print(SYSROOT)


if __name__ == "__main__":
    main()
