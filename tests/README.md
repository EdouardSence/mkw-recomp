# tests

Tests that need neither the game nor a GPU, run by the CI on Linux and Windows (`.github/workflows/ci.yml`). The back
end's regression test runs over captures instead (`tools/test/`).

| File | What it checks |
|---|---|
| `test_setup.py` | The setup's command lines and files from folders with spaces and accents: the compiler's path on its way to the shader step (issue #29), the resume without `--source` after an update, the update's version order and its clean-up of `*.old` files |

Usage: `uv run pytest tests`
