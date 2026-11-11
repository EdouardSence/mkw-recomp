# sm86/decode

Decoder of 128-bit SASS words: `sass_decode.decode(spec, word)` returns the instruction class (`.mnemonic`, `.cls`) and
the value of each field (`.values`). The specification (`sass_spec.Spec(path)`) is `build/sm86/instructions.json`,
which [`sm86/spec/spec.py`](../spec/README.md) generates from NVIDIA's `nvdisasm`; it is neither versioned nor shipped.

Copied from [nvsass-disassembler](https://github.com/reoLantern/nvsass-disassembler) @ 28f9064 (MIT, license in
`LICENSE.nvsass-disassembler`): `sass_spec.py` and `sass_decode.py`, changed only for speed with the same output
(11/11/2026: a slot read a run of bits at a time, each enum's names indexed by value; 300 000 distinct MKW words
60.9 → 36.6 s, every decode identical).

Validated with `tools/shaders/decode_check.py` and `operand_check.py` (06/11/2026): the 5 552 888 instructions of MKW's
11 561 programs give the same opcode as `nvdisasm`, all with a single possible class, and the same registers,
predicates, bank operands and signs.
