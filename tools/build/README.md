# tools/build

| Script | What it does | Usage |
|---|---|---|
| `icon.py` | Draws `mkw.exe`'s icon in code (our own chequered flag, no game art) into a `.ico` of 256, 48, 32 and 16 px; `build.py --target windows` calls it before the `runtime/platform/mkw.rc` resource | `uv run tools/build/icon.py OUT.ico` |
| `windeps.py` | Downloads to `build/win` what the Windows `.exe` needs: llvm-mingw 20261020 (`build/win/llvm-mingw`) and the headers, libraries and DLLs of SDL3, Opus, libvpx, zstd, xxHash, Vulkan and VMA from MSYS2 UCRT64 (`build/win/deps`), each download checked against its sha256 | `uv run tools/build/windeps.py` (~1 min; 670 MB) |
| `dist.py` | Assembles the player's folder in `build/dist/<target>` after `build.py --dist`: `setup/` with a portable CPython (python-build-standalone) with numpy and lz4, zig 0.17.1, `glslc` from conda-forge and the sources the setup runs (`tools/prepare/prepare.py`); the start screen creates the desktop shortcut (`.desktop` on Linux, `.lnk` on Windows). Each download checked against its sha256 | `uv run tools/build/dist.py [--target linux\|windows] [--check]` |
| `build.py` | Writes `build/obj/build.ninja` and builds with Ninja the C of `build/generated` (`build/obj/lib{main,subsdk0,sdk}.a`) and the runtime, and links `build/mkw`. Writes `build/obj/rt/modules.c` (the game tables of `runtime/core/boot/gamelib.h`: modules, function tables, ExeFS hashes, patch sets) and `defaults.c` (the copy's paths). `--dist`: the player's build in `build/dist/<target>`, the runtime (`mkw`, `mkw.exe`) and separately the game library (`mkwgame.so`, `mkwgame.dll`), which the runtime loads from its folder (objects in `build/obj-dist[-win]`; the library links with `cc`, without libstdc++). Parallel jobs come from a memory budget. The C goes through `ccache` when installed (`MKW_CCACHE=0` turns it off): moving folders changes every `-I` of the runtime and a comment in `runtime/cpu` rebuilds the whole game. `--frame-pointers`: runtime with frame pointers for `perf -g` stacks (where each thread blocks: `-e cs -g`). `--target windows`: `build/mkw.exe` from Linux with llvm-mingw (objects in `build/obj-win`; window, input, keyboard and menu through SDL3 as on Linux, `platform/win32.c` instead of `linux.c`) | `uv run tools/build/build.py` (~7 min from scratch; seconds after) |

Before: `uv run python recomp/gen.py --exefs game/exefs --out build/generated --jobs 8 --patches recomp/patches-mkw.txt`
(generates the C).

Needs `clang`, `lld`, `ninja`, `sdl3`, `opus`, `libvpx`, `zstd`, `xxhash`, `vulkan-headers` and the VMA 3.4.0 header (on Arch
it is only in the AUR):

```sh
mkdir -p scratchpad/vma && curl -sSfL -o scratchpad/vma/vk_mem_alloc.h \
  https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/v3.4.0/include/vk_mem_alloc.h
# sha256 8487b7995ad3b263eb73bc5b9a77d71aa69b6bef5d58a715c02d2663afd81f1a
```

Run: `build/mkw --boot` (also `--selftest`, `--probe-loader`).

Without the game: `uv run tools/build/build.py --dist --runtime-only [--target windows]` builds only the runtime with
the start screen (`build/dist/<target>/mkw`), what a release ships and the CI checks.

## Windows

`build/mkw.exe` is built from Linux:

```sh
uv run tools/build/windeps.py
uv run tools/build/build.py --target windows   # ~7 min from scratch
tools/run/wine.sh build/mkw.exe --selftest
```

To play on Windows, next to `mkw.exe`: the DLLs of `build/win/deps/bin` (except `libxxhash.dll`), `spirv/`, and the
game copy in `exefs/` and `romfs/` (or `--exefs`/`--romfs`). It needs a driver with Vulkan 1.3 (without
`VK_EXT_mesh_shader`, task and mesh shaders run as compute: `MKW_MESH_COMPUTE`) and a CPU with AVX2
(`-march=x86-64-v3`).
