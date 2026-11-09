# docs/releases

Release notes. `template.md` holds what every release repeats (install, updating, known issues, reporting a problem);
`v<version>.md` only what that release adds: a summary paragraph, `## Changes` (each with its issue, #N), `## Tested`
(what was run from the packages before publishing, on which systems and GPUs) and optionally `## Known issues`.
`tools/build/notes.py --version X.Y.Z` puts them together for `gh release create --notes-file`.

| File | What it is |
|---|---|
| `template.md` | The common part, with `{summary}`, `{changes}`, `{tested}` and `{known}` filled from the version's file |
