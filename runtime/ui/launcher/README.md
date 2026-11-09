# runtime/ui/launcher

The start screen, before the game, and the setup of the game files.

| File | What it is |
|---|---|
| `launcher.cpp`, `launcher.h` | Start screen before the game (Play, Settings, Game files, Quit) in its own window with SDL_Renderer; always in the player's build, with `--launcher` in `build/mkw`. Test: `MKW_LAUNCHER_SHOT=<frame>[:<page>[:<tab>]]` |
| `launcher_setup.cpp`, `launcher_setup.h` | The setup of the files page: `tools/prepare/prepare.py` in a child process (SDL_Process) with the package's Python, and its progress line by line |
