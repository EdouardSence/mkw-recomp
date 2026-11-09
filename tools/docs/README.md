# tools/docs

Keeping the docs from going stale: what used to be copied by hand is generated, and the rest is checked at commit time.

| Script | What it does | Usage |
|---|---|---|
| `check.py` | Called by `tools/git/pre-commit` and the CI; rejects the commit if a folder has no README (except third-party code with its `LICENSE` and `.github/`), a README does not name one of its files or subfolders, a relative link of a `.md` does not exist, the README's progress bars are not the generated ones, or a translator change does not carry its measurements | `uv run tools/docs/check.py` |
| `progress.py`, `progress.json` | Progress bars of the root README (cog block): NVN2 implemented, `main`'s `nn::*` imports replaced and tested, SM86 opcodes translated. What is done is counted in the code; the game's totals (names, imports, census, failures) are in `progress.json`, which `--update` rebuilds from `build/` | `uv run tools/docs/progress.py --update` and `uv run cog -r -I tools/docs README.md` |
