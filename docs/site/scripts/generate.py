#!/usr/bin/env python3
"""Runs every generator, in order: header extraction, design notes, coverage.

The MkDocs hook (hooks/equine_docs.py) calls this before each build, so a
plain `mkdocs build` is always up to date; CI also runs it explicitly.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import coverage_report  # noqa: E402
import design_notes  # noqa: E402
import extract_headers  # noqa: E402


def main() -> int:
    for step in (extract_headers.main, design_notes.main, coverage_report.main):
        if step() != 0:
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
