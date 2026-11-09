# runtime/core/base

The runtime's common pieces: the `host.h` interface, settings (`cfg`), commands to a running game, system info, hashing
and UTF-16.

| File | What it is |
|---|---|
| `host.h` | The runtime's common interface |
| `cfg.c`, `cfg.h` | Settings (ini file and environment variables). `cfg_bool`/`cfg_value` require a row in the settings table (without one, fatal, also under `--selftest`); diagnostic `MKW_*` options without a row are read with `cfg_get` |
| `ctl.c` | `MKW_CTL=<fifo>`: commands to the running game (screenshot, dump, flight recorder, button presses, textures at an address, game memory to a file); `tools/run/ctl.sh` |
| `sysinfo.c`, `sysinfo.h`, `sha256.c`, `sha256.h`, `utf16.h` | System info, hashing, UTF-16 |
