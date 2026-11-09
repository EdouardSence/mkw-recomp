# runtime/sdk

Replacements of the SDK's `nn::*` functions by name (`ov_*.c`, "override"). Every subfolder with headers is on the
include path (`tools/build/build.py`): `#include` goes by name.

| Folder | Contents |
|---|---|
| [ov/](ov/README.md) | Replacements: system, input, audio, video, graphics, network, time, touch |
| [fs/](fs/README.md) | Files, saves and RomFS, and the internal resolution overlay |
| [swkbd/](swkbd/README.md) | Software keyboard |
