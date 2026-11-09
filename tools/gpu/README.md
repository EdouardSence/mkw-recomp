# tools/gpu

| File | What it is | Usage |
|---|---|---|
| `nvn_usage.py` | Which NVN2 functions the game uses: pairs each name of `main`'s proc loader with the global where it stores the pointer and counts the functions that read it. A lower bound (it does not see accesses through a pointer to the table). Writes `build/nvn_usage.csv` | `uv run tools/gpu/nvn_usage.py` (needs `build/bindb.sqlite`) |
| `nvn1_procs.txt` | The 537 NVN1 (Switch 1) names, from totk-pc-recomp `runtime/nvn_procs.inc` @ c3b9a4e (MIT) | Read by `nvn_usage.py` to mark the new ones |
| `nvn2_procs.py` | Address of the 698 NVN2 functions inside `sdk`, read from the driver's own table (sorted names at `sdk+0xb79b90`, pointers at `sdk+0xd95168`; 1.0.0 offsets). `bindb.py` uses it to name those functions | `uv run tools/gpu/nvn2_procs.py` |
| `nvn2_sizes.py` | Lower bound of each NVN2 object's size (largest offset the driver touches on `x0`) next to its NVN1 size | `uv run tools/gpu/nvn2_sizes.py` (needs `scratchpad/ext/totk`) |
| `gen_nvn2_procs.py` | Writes `runtime/gfx/nvn/procs/nvn2_procs.inc`: the NVN2 procs NVN1 lacks, so each has a stub that stops with its name | `uv run tools/gpu/gen_nvn2_procs.py` |
| `gen_nvn2_devinfo.py` | Writes `runtime/gfx/nvn/procs/nvn2_devinfo.inc`: the 170 answers of the NVN2 driver's `nvnDeviceGetInteger` (version 231.16); the device object's fields keep the NVN1 value (`nvn1_devinfo.txt`) | `uv run tools/gpu/gen_nvn2_devinfo.py` |
| `nvn1_devinfo.txt` | The NVN1 (Tegra X1) table of totk-pc-recomp's front end, as a reference for the previous one | — |
| `sparse_fit.py` | Where the game maps the 64 KiB pages of each sparse texture, against the assumed layout (layer = levels with whole tiles, tiles in rows; tails apart). The layout of `runtime/gfx/vk/tex/vk_sparse.c` came from it | `uv run tools/gpu/sparse_fit.py build/run/NAME.log [--tex ADDR]` on a run with `MKW_SPARSE_TRACE=1` |

Reference sources are cloned into `scratchpad/ext/` (`git clone https://github.com/valentderah/totk-pc-recomp
scratchpad/ext/totk`).
