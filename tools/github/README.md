# github

Issue moderation, run by `.github/workflows/issues.yml` on each issue event, once a day and on each release.

| File | Contents |
|---|---|
| `moderate.py` | Acts on an issue, a comment, a release or the daily sweep: release and GPU labels, empty reports closed, logs asked for, known errors answered, maintainers' replies closed. Prints what it would do unless `--apply` |
| `classify.py` | Reads a report without GitHub: release, GPU, empty, log, known error, and what a maintainer's reply decides (tested in `tests/test_issues.py`) |
| `signatures.toml` | Known error strings and the issue that tracks each; `fixed` keeps reports on that release or later open as possible regressions |

A maintainer's reply closes its issue when it says "Fixed in vX" opening a sentence (or "is the same as #N, fixed in
vX") with vX published, or "is the same as #N" (closed as its duplicate). "Fixed in" a release not yet out labels it
`fixed-next-release`, closed when that release is published.

Usage: `uv run tools/github/moderate.py backlog` (all open issues, dry run), `... issue N --apply`
