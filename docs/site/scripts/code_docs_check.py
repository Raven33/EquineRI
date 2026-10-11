#!/usr/bin/env python3
"""Checks the code documentation rules of docs/CodeDocumentation.md.

Fails (exit 1) when:
  1. a source file has no header comment: after an optional `#pragma once`,
     the first line must be a `//` comment (`#` or a docstring for Python);
  2. a comment, ARCHITECTURE.md or docs/CodeDocumentation.md names a decision
     (P1, P2, ...) that is not in CONTEXT.md.

Run with the docs build (scripts/generate.py); standard library only.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import design_notes  # noqa: E402
from docs_common import CONTEXT_MD, REPO_DIR, SNIPPETS_DIR  # noqa: E402

SOURCE_DIRS = ("src", "backends", "helpers", "tests", "samples", "tools", "shaders")
SOURCE_SUFFIXES = {".cpp", ".hpp", ".h", ".hlsl", ".hlsli", ".py"}
MARKDOWN = (REPO_DIR / "ARCHITECTURE.md", REPO_DIR / "docs" / "CodeDocumentation.md")
DECISION = re.compile(r"\bP(\d+)\b")


def source_files() -> list[Path]:
    roots = [REPO_DIR / d for d in SOURCE_DIRS] + [SNIPPETS_DIR]
    files = []
    for root in roots:
        files += [p for p in root.rglob("*") if p.suffix in SOURCE_SUFFIXES and p.is_file()
                  and "__pycache__" not in p.parts]
    return sorted(files)


def has_header(path: Path, lines: list[str]) -> bool:
    rest = [line for line in lines if line.strip()]
    if rest and rest[0].startswith("#pragma once"):
        rest = rest[1:]
    if not rest:
        return False
    if path.suffix == ".py":
        if rest[0].startswith("#!"):
            rest = rest[1:]
        return bool(rest) and (rest[0].startswith("#") or rest[0].lstrip().startswith(('"""', "'''")))
    return rest[0].startswith("//")


def comment_text(path: Path, lines: list[str]) -> list[str]:
    marker = "#" if path.suffix == ".py" else "//"
    return [line[line.index(marker):] for line in lines if marker in line]


def main() -> int:
    known = {number for number, _, _ in design_notes.decisions(CONTEXT_MD.read_text(encoding="utf-8"))}
    problems: list[str] = []
    for path in source_files():
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        rel = path.relative_to(REPO_DIR).as_posix()
        if not has_header(path, lines):
            problems.append(f"{rel}: no header comment")
        for text in comment_text(path, lines):
            for match in DECISION.finditer(text):
                if int(match.group(1)) not in known:
                    problems.append(f"{rel}: names P{match.group(1)}, which is not in CONTEXT.md")
    for path in MARKDOWN:
        for match in DECISION.finditer(path.read_text(encoding="utf-8")):
            if int(match.group(1)) not in known:
                problems.append(f"{path.relative_to(REPO_DIR).as_posix()}: names P{match.group(1)}, "
                                "which is not in CONTEXT.md")
    for problem in sorted(set(problems)):
        print(f"code_docs_check: {problem}")
    print(f"code_docs_check: {len(source_files())} source files, {len(set(problems))} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
