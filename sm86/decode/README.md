# sm86/decode

Decoder of 128-bit SASS words: `sass_decode.decode(spec, word)` returns the instruction class (`.mnemonic`, `.cls`) and
the value of each field (`.values`). The specification (`sass_spec.Spec(path)`) is `build/sm86/instructions.json`,
which `tools/shaders/spec.sh` generates from a local `nvdisasm`; it is not versioned.

Copied from [nvsass-disassembler](https://github.com/reoLantern/nvsass-disassembler) @ 28f9064 (MIT, license in
`LICENSE.nvsass-disassembler`): `sass_spec.py` and `sass_decode.py`, unchanged.

Validated with `tools/shaders/decode_check.py` and `operand_check.py` (06/11/2026): the 5 552 888 instructions of MKW's
11 561 programs give the same opcode as `nvdisasm`, all with a single possible class, and the same registers,
predicates, bank operands and signs.
