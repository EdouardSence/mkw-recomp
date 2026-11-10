# Changelog

Each version's section is that release's notes: `tools/build/notes.py` puts it into `docs/release-template.md` (install,
updating, known issues, reporting) for the GitHub release. A section has a summary paragraph, `### Changes` (each with
its issue, #N), `### Tested` (what was run from the packages before publishing) and optionally `### Known issues`.

## 0.1.5

Local multiplayer, the game no longer runs in slow motion below 60 fps, and the start screen now updates itself.

### Changes

- Local multiplayer with one controller per player (#23): each connected controller is its own player (the first one
  and the keyboard are player 1), the console's controller screen gets the number connected, and rumble goes to each
  player's own controller. Before, Multiplayer went back to the home screen.
- Below 60 fps the race ran in slow motion with Frame rate on Automatic: the game picks 30 or 60 fps from the time the
  GPU reports for each frame, and ours reported the time the work was sent, so it kept choosing 60. It now reports when
  the GPU finishes, and the game stays at 30 (on a Radeon 660M: 110 switches between 30 and 60 in 20 s of race to 0).
- The start screen offers a newer release and installs it (Settings > Menu > Check for updates turns it off).
- The 4096 shadow map (Settings > Graphics) no longer crashes at the kart screen (#9).
- On scaled displays the mouse no longer lands off its place on the start screen (#27).

### Tested

From these packages, before publishing:

- The whole setup on Linux (Radeon 660M, RADV) and on Windows 11 (GTX 1050 Ti), and the update from the start screen on
  both.

## 0.1.4

Fixes the setup that stopped at 359/360 on many Windows PCs, and the shader step failing when a worker runs out of
memory.

### Changes

- "Compile the game code" stopped at 359/360 on Windows (#2, #12): zig, the C compiler in the package, builds its own C
  runtime the first time it links, and on those PCs that build never ended (`setup.log` had no `link probe` line). The
  package now carries that runtime already built and the setup links with it, so zig builds nothing of its own on the
  player's PC, neither for the game library nor for the shader step's helper DLL, where #24 stopped after 360/360.
- The shader step failed with `BrokenProcessPool` when the system ended a worker, usually when memory ran out (#24): it
  now goes on with half the workers, down to one, and `setup.log` says so.

### Tested

From these packages, before publishing:

- The whole setup (generate, compile, translate the shaders, the first run of the game) on Linux (Radeon 660M, RADV)
  with no system tools on PATH, and on Windows 11 (GTX 1050 Ti) with zig's cache emptied first.

## 0.1.3

Fixes two ways the setup could fail or look stuck and a GPU crash on NVIDIA under Linux, keeps a log of the setup, and
makes a GPU crash say where it happened.

### Changes

- "Compile the game code" sat at 359/359 with no sign of life (#2): after the last file the game library is linked,
  and on its first link zig also builds its C runtime for the target (76 s on a 6-thread Windows PC, more on a slow
  disk or with an antivirus scanning). That now runs while the files compile, the link counts in the progress bar,
  and a link that takes over 30 minutes stops with an error instead of waiting forever.
- `unable to find zig installation directory`: zig now gets its `lib/` folder named, so it no longer depends on the
  path a launcher or sandbox gives its executable.
- The setup writes `logs/setup.log`: every tool's output and how long each step took; a failed setup points to it.
- NVIDIA on Linux lost the device as soon as sparse textures were in use (at boot or when a race started): a texture
  was destroyed while a bind of its memory was still queued (#7, thanks to @guillo1990; likely #6).
- When the GPU is lost (#4, #5: AMD discrete cards that went black after the startup logo), `logs/mkw.log` now says
  what the driver knows (the faulting address, `VK_EXT_device_fault`) and which upload batch, render pass or dispatch
  the GPU had started and finished last (`VK_AMD_buffer_marker` marks). Nothing to set: attach the log.

### Tested

From these packages, before publishing:

- The whole setup (generate, compile, translate the 11 561 shaders, the first run of the game) on Linux (Radeon 660M,
  RADV) with no system tools on PATH, and on Windows 11 (GTX 1050 Ti).
- Races on both with the GPU marks on and off (no measurable cost), and #7's fix on AMD (a 60 s race, no GPU
  faults).

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
