# runtime/sdk/fs

Files, saves and RomFS, and the RomFS overlay with the internal resolution.

| File | What it is |
|---|---|
| `fs_host.c`, `fs.h`, `romfs.c`, `romfs_path.h` | Files, saves and RomFS (with `ROMFS_LOOSE`, the dump's loose `romfs/` folder) |
| `romfs_res.c` | Internal resolution (`MKW_RESOLUTION`, the original by default): at boot writes the RomFS overlay with the chosen lines to `cache/resolution-<lines>/`, as `tools/romfs/resolution.py` does |
