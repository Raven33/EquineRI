"""Shared helpers for the documentation scripts and the MkDocs hook.

Standard library only. Paths are resolved from this file, so every script works
from any current directory.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

SITE_DIR = Path(__file__).resolve().parent.parent  # docs/site
REPO_DIR = SITE_DIR.parent.parent  # repository root
DOCS_DIR = SITE_DIR / "docs"  # MkDocs docs_dir
SNIPPETS_DIR = SITE_DIR / "snippets"
GENERATED_DIR = SITE_DIR / "generated"  # includes only, never pages
SIGNATURES_DIR = GENERATED_DIR / "signatures"
API_JSON = GENERATED_DIR / "api.json"
CONTEXT_MD = REPO_DIR / "CONTEXT.md"

# Generated pages inside docs_dir (git-ignored, rebuilt on every build).
DECISIONS_PAGE = DOCS_DIR / "design-notes" / "decisions.md"
COVERAGE_PAGE = DOCS_DIR / "about" / "coverage.md"
GENERATED_PAGES = (DECISIONS_PAGE, COVERAGE_PAGE)

# Languages other than the default (English). A translation of `page.md` is
# `page.<lang>.md`; untranslated pages fall back to English.
TRANSLATIONS = ("fr",)

# A pymdownx.snippets include line: --8<-- "path" or --8<-- "path:section".
# A leading ';' escapes it (shown literally), as in pymdownx.
SNIPPET_LINE = re.compile(r'^(?P<indent>[ \t]*)(?P<escape>;*)-{1,}8<-{1,}[ \t]+"(?P<ref>[^"\n]+)"[ \t]*$')
SECTION_SUFFIX = re.compile(r"^(?P<path>.*?):(?P<section>[a-z][-_0-9a-z]*)$")
SECTION_MARKER = re.compile(r"-{1,}8<-{1,}[ \t]+\[[ \t]*(?P<type>start|end)[ \t]*:[ \t]*(?P<name>[a-z][-_0-9a-z]*)[ \t]*\]")

KIND_LABELS = {
    "ri": "RI",
    "vulkan": "Vulkan",
    "d3d12": "D3D12",
    "hlsl": "HLSL",
    "signature": "Signature",
    "other": "Include",
}


@dataclass(frozen=True)
class SnippetRef:
    ref: str  # as written in the page
    path: str  # file part, relative to the snippet base (docs/site)
    section: str | None

    @property
    def kind(self) -> str:
        if self.path.startswith("snippets/ri/"):
            return "ri"
        if self.path.startswith("snippets/vulkan/"):
            return "vulkan"
        if self.path.startswith("snippets/d3d12/"):
            return "d3d12"
        if self.path.startswith("snippets/hlsl/"):
            return "hlsl"
        if self.path.startswith("generated/signatures/"):
            return "signature"
        return "other"


def parse_ref(ref: str) -> SnippetRef:
    m = SECTION_SUFFIX.match(ref)
    if m:
        return SnippetRef(ref, m.group("path"), m.group("section"))
    return SnippetRef(ref, ref, None)


def file_sections(path: Path) -> set[str]:
    """Names of sections that have both a start and an end marker."""
    starts: set[str] = set()
    ends: set[str] = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        m = SECTION_MARKER.search(line)
        if m:
            (starts if m.group("type") == "start" else ends).add(m.group("name"))
    return starts & ends


def missing_reason(ref: SnippetRef, base: Path = SITE_DIR) -> str | None:
    """None when the include resolves; otherwise why it does not."""
    target = (base / ref.path).resolve()
    if base.resolve() not in target.parents or not target.is_file():
        return f"file `{ref.path}` does not exist"
    if ref.section is not None and ref.section not in file_sections(target):
        return f"section `{ref.section}` is not in `{ref.path}`"
    return None


def snippet_refs(markdown: str) -> list[SnippetRef]:
    """Every (unescaped) include in a page, in order."""
    refs = []
    for line in markdown.splitlines():
        m = SNIPPET_LINE.match(line)
        if m and not m.group("escape"):
            refs.append(parse_ref(m.group("ref")))
    return refs


def split_front_matter(text: str) -> tuple[dict[str, object], str]:
    """A small YAML subset: `key: value` and `key: [a, b]` lines only."""
    if not text.startswith("---\n"):
        return {}, text
    end = text.find("\n---\n", 4)
    if end < 0:
        return {}, text
    meta: dict[str, object] = {}
    for line in text[4:end].splitlines():
        if ":" not in line or line.startswith((" ", "\t", "#")):
            continue
        key, value = line.split(":", 1)
        value = value.strip()
        if value.startswith("[") and value.endswith("]"):
            meta[key.strip()] = [v.strip().strip("'\"") for v in value[1:-1].split(",") if v.strip()]
        else:
            meta[key.strip()] = value.strip("'\"")
    return meta, text[end + 5 :]


def is_translation(path: Path) -> bool:
    parts = path.name.split(".")
    return len(parts) >= 3 and parts[-2] in TRANSLATIONS


def english_pages() -> list[Path]:
    """Every default-language page in docs_dir, generated ones included."""
    return sorted(p for p in DOCS_DIR.rglob("*.md") if not is_translation(p))


def write_if_changed(path: Path, content: str) -> bool:
    """Writes only on change, so `mkdocs serve` does not rebuild in a loop."""
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_text(encoding="utf-8") == content:
        return False
    path.write_text(content, encoding="utf-8")
    return True


def symbol_file_name(symbol: str) -> str:
    """RI::Vk::createDevice -> RI.Vk.createDevice.md"""
    return symbol.replace("::", ".") + ".md"
