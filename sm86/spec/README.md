# sm86/spec

The SM86 encoding specification the decoder reads (`instructions.json`), built on the machine that uses it: it is
NVIDIA's own text, held in memory by `nvdisasm`, so it is neither versioned nor shipped. The player's setup
(`tools/prepare/prepare.py`, into `<game>/sm86/`) and a source checkout (into `build/sm86/`) run `spec.py`.

| File | What it is |
|---|---|
| `spec.py` | Fetches CUDA 12.8's redistributable `nvdisasm` and `ptxas` (pinned by sha256), assembles a minimal sm_86 cubin, runs `nvdisasm` with `intercept.c` preloaded, cuts the capture with `funnel.py` and parses it with `parse_sidl.py`; retries a garbled capture. On Windows it loads `intercept_win.c` as a DLL into the suspended `nvdisasm.exe` instead of preloading `intercept.c`. `python sm86/spec/spec.py [--out build/sm86] [--cache scratchpad/nvidia] [--cc CMD]` (~1 min) |
| `intercept_win.c` | Ours: Windows' `nvdisasm.exe` carries its C runtime inside, so this DLL rewrites the exe's own `memcpy` (checked by its first bytes, at a fixed RVA of the sha256-pinned exe) to print what `intercept.c` prints |
| `intercept.c`, `funnel.py` | From [DocumentSASS](https://github.com/0xD0GF00D/DocumentSASS) @ c97efa9 (MIT, `LICENSE.DocumentSASS`), unchanged |
| `parse_sidl.py`, `sidl_lex.py`, `sidl_decls.py`, `sidl_class.py` | From [nvsass-disassembler](https://github.com/reoLantern/nvsass-disassembler) @ 28f9064 (MIT, `LICENSE.nvsass-disassembler`), unchanged |
