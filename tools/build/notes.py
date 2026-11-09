"""A release's notes: docs/releases/template.md (install, updating, known issues, reporting: the same in every release)
filled with that version's docs/releases/v<version>.md (summary, changes, what was tested, extra known issues).

Why: v0.1.0-v0.1.2 were written by hand, each with its own shape and the same install text copied and drifting.

The version file has a first paragraph (the summary) and the sections "## Changes", "## Tested" and, optionally,
"## Known issues" (lines added under the template's). Each change names its issue (#N) when it has one.

Usage: uv run tools/build/notes.py --version 0.1.3 [--out FILE]   (default: stdout; gh release create --notes-file FILE)
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DOCS = ROOT / "docs/releases"
SECTIONS = {"changes": "Changes", "tested": "Tested", "known": "Known issues"}


def parse(text: str, name: str) -> dict[str, str]:
    """The summary and the sections of a version file."""
    parts = re.split(r"^## (.+)$", text.strip(), flags=re.M)
    got = {"summary": parts[0].strip()}
    titles = {v: k for k, v in SECTIONS.items()}
    for title, body in zip(parts[1::2], parts[2::2], strict=True):
        if title.strip() not in titles:
            sys.exit(f"{name}: unknown section '## {title.strip()}' (expected: {', '.join(SECTIONS.values())})")
        got[titles[title.strip()]] = body.strip()
    for key in ("summary", "changes", "tested"):
        if not got.get(key):
            sys.exit(f"{name}: no {'summary paragraph' if key == 'summary' else '## ' + SECTIONS[key]}")
    return got


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--version", required=True, help="e.g. 0.1.3 (the tag without its v)")
    ap.add_argument("--out", type=Path, help="write here instead of stdout")
    args = ap.parse_args()
    src = DOCS / f"v{args.version}.md"
    if not src.is_file():
        sys.exit(f"{src} missing: write the version's summary, changes and tests there first")
    got = parse(src.read_text(encoding="utf-8"), src.name)
    notes = (
        (DOCS / "template.md")
        .read_text(encoding="utf-8")
        .format(summary=got["summary"], changes=got["changes"], tested=got["tested"], known=got.get("known", ""))
    )
    notes = re.sub(r"\n{3,}", "\n\n", notes).strip() + "\n"
    if args.out:
        args.out.write_text(notes, encoding="utf-8")
    else:
        sys.stdout.write(notes)


if __name__ == "__main__":
    main()
