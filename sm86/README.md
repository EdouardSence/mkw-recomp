# sm86

Translator of the game's shaders (Ampere SM86 SASS) to SPIR-V, offline.

| Folder | Contents |
|---|---|
| [decode/](decode/README.md) | Instruction decoder driven by NVIDIA's encoding specification |
| [spec/](spec/README.md) | Builds that specification on this machine from NVIDIA's `nvdisasm` (never versioned nor shipped) |
| [translate/](translate/README.md) | SM86 → GLSL → SPIR-V translator |
