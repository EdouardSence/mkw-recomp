# runtime/ui/launcher

The start screen, before the game, and the setup of the game files.

| File | What it is |
|---|---|
| `launcher.cpp`, `launcher.h` | Start screen before the game (Play, Settings, Game files, Quit) in its own window with SDL_Renderer; always in the player's build, with `--launcher` in `build/mkw`. Test: `MKW_LAUNCHER_SHOT=<frame>[:<page>[:<tab>]]` |
| `launcher_backdrop.cpp`, `launcher_backdrop.h` | The start screen's background: the player's `splash.png` scaled to cover, else a road drawn by code |
| `launcher_setup.cpp`, `launcher_setup.h` | The setup of the files page: `tools/prepare/prepare.py` in a child process (SDL_Process) with the package's Python, and its progress line by line |
| `launcher_update.cpp`, `launcher_update.h` | The update: at start (`MKW_UPDATE_CHECK`, Menu tab), GitHub's latest published release offered on the start screen; `tools/prepare/update.py` downloads it, then the new files replace the old ones (Windows: renamed to `*.old`; Linux: the AppImage) and the program starts again, which sets up again only what changed. Test: `MKW_UPDATE_AS=<version> MKW_LAUNCHER_UPDATE=1` |
