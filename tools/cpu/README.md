# tools/cpu

| Script | What it does | Usage |
|---|---|---|
| `lift_coverage.py` | Translates every distinct word of the `.text` of `main`, `sdk` and `subsdk0` with `recomp/lifter.py` and groups what it does not translate by mnemonic (capstone), with its frequency. It is the CPU's work list | `uv run tools/cpu/lift_coverage.py [--json scratchpad/lift_coverage.json]` (~1 min on all cores) |
| `lift_diff.py` | Differential test of the lifter's new classes (LSE atomics, CAS/CASP, LDAPR, XPAC, RETAA/RETAB) against Unicorn with the `max` CPU: equal registers and memory. `LDSMAX`/`LDSMIN` in memory against ARM's pseudocode (Unicorn compares unsigned). Not PAC/AUT: here they are the identity on purpose | `uv run --with unicorn tools/cpu/lift_diff.py [--n 5000]` (~1 min; 5 000 of 5 000 on 06/11/2026) |
| `self_loops.py` | Counts the `jmp`s to themselves in `build/mkw` against the game's self loops in the generated C, per function; functions with more `jmp`s than game loops come from clang and need a look | `uv run tools/cpu/self_loops.py` (~2 min) |
| `spin_waits.py` | Finds the spin loops of `main`, `subsdk0` and `sdk` (loops only another thread can end) and writes them as `spin` lines of the patch table. From totk-pc-recomp @ c3b9a4e, which adapts it from nxnative (MIT) | `uv run tools/cpu/spin_waits.py --exefs game/exefs > recomp/patches-mkw.txt` (~1 min; 117 on 30/10/2026). Needs capstone |

Last result (30/10/2026): only padding `udf`s and 131 data words are left.
