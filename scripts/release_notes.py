#!/usr/bin/env python3
"""Print the CHANGELOG.md section for one version.

    python scripts/release_notes.py v0.2.2            # full section body
    python scripts/release_notes.py v0.2.2 --summary  # one line for the firmware manifest

Exits non-zero when the section is missing, which is what makes a tag without
release notes fail CI.
"""

import re
import sys
from pathlib import Path

CHANGELOG = Path(__file__).resolve().parent.parent / "CHANGELOG.md"


def section(version: str) -> str:
    version = version if version.startswith("v") else "v" + version
    text = CHANGELOG.read_text(encoding="utf-8")
    m = re.search(rf"^## {re.escape(version)}\s*$(.*?)(?=^## |\Z)", text, re.M | re.S)
    if not m or not m.group(1).strip():
        raise SystemExit(f"CHANGELOG.md has no '## {version}' section. Add one before tagging.")
    return m.group(1).strip() + "\n"


def summary(body: str, limit: int = 240) -> str:
    """Bullets joined into one line, trimmed to fit an ESPHome text field."""
    bullets = [ln[2:].strip() for ln in body.splitlines() if ln.startswith("- ")]
    out = " ".join(bullets) if bullets else body.strip().splitlines()[0]
    return out if len(out) <= limit else out[: limit - 3].rstrip() + "..."


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    body = section(sys.argv[1])
    print(summary(body) if "--summary" in sys.argv else body, end="")
