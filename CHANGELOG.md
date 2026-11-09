# Changelog

Each version's section is that release's notes: `tools/build/notes.py` puts it into `docs/release-template.md` (install,
updating, known issues, reporting) for the GitHub release. A section has a summary paragraph, `### Changes` (each with
its issue, #N), `### Tested` (what was run from the packages before publishing) and optionally `### Known issues`.

## 0.1.3

Fixes two ways the setup could fail or look stuck and a GPU crash on NVIDIA under Linux, keeps a log of the setup, and
makes a GPU crash say where it happened.

### Changes

- "Compile the game code" sat at 359/359 with no sign of life (#2): after the last file the game library is linked,
  and on its first link zig also builds its C runtime for the target (76 s on a 6-thread Windows PC, more on a slow disk or with an
  antivirus scanning). That now runs while the files compile, the link counts in the progress bar, and a link that
  takes over 30 minutes stops with an error instead of waiting forever.
- `unable to find zig installation directory`: zig now gets its `lib/` folder named, so it no longer depends on the
  path a launcher or sandbox gives its executable.
- The setup writes `logs/setup.log`: every tool's output and how long each step took; a failed setup points to it.
- NVIDIA on Linux lost the device as soon as sparse textures were in use (at boot or when a race started): a texture
  was destroyed while a bind of its memory was still queued (#7, thanks to @guillo1990; likely #6).
- When the GPU is lost (#4, #5: AMD discrete cards that went black after the startup logo), `logs/mkw.log` now says
  what the driver knows (the faulting address, `VK_EXT_device_fault`) and which upload batch, render pass or dispatch
  the GPU had started and finished last (`VK_AMD_buffer_marker` marks). Nothing to set: attach the log.

### Tested

TESTED-PENDING

## 0.1.2

Fixes the setup, which could not build the game in 0.1.0 and 0.1.1.

### Changes

- It stopped right after "Generate the game code" with `fatal error: 'gamelib.h' file not found` (#1), on Linux and
  Windows.
- Translating the shaders needed `glslc` and `spirv-val` installed on the system.

### Tested

The whole setup (generate, compile, shaders and the first run of the game) from these packages on Linux, with no
system tools on PATH, and on Windows.

## 0.1.1

The Windows zip; its setup builds the shader spec too.

### Changes

- Windows 10/11 x64 package.

### Tested

The start screen on Linux and Windows.

## 0.1.0

First release: the start screen for Linux as an AppImage.

### Changes

- Linux AppImage of the start screen and its setup tools.

### Tested

The start screen on Linux.
