# Performance

Frame times measured on real machines, each with its date: the code changes every week, so an old row says what that
build did, not what today's does. MKW asks for 60 fps in races (16.7 ms per frame); on the console, when a frame takes
longer, the game itself drops to 30 fps (33.3 ms). Here the Automatic frame rate does not settle at 30 yet: it flips
between 60 and 30 and the race runs in slow motion (0.54x on the Radeon 660M). Settings > Screen > Frame rate > 30
fixes it at 30 (0.89x on the same machine, which does not quite hold 33.3 ms). A frame time over 33.3 ms means slow
motion even at 30: the game counts each frame as 1/30 s. The log's `game: speed` lines give the game speed of a run.

To add your machine, open a [performance report](https://github.com/adriadam10/mkw-recomp/issues/new?template=performance.yml);
it is added here with the issue's date and a link to it.

| Date | Version | GPU (driver) | CPU | OS | Internal resolution | Scene | Frame time | Source |
|---|---|---|---|---|---|---|---|---|
| 09/11/2026 | pre-release | Radeon 660M (Mesa 26.2 RADV) | Ryzen 5 7535HS, 13 GB | Linux 7.2 (CachyOS) | 720p | Race | 31-33 ms median | maintainer |
| 09/11/2026 | pre-release | GTX 1050 Ti 4 GB (NVIDIA 582.66) | Core i5-8400, 16 GB | Windows 11 | 1080p | Race | 42-45 ms median | maintainer |
