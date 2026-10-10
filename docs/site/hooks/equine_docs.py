"""MkDocs hook for the EquineRI site.

1. on_config: points pymdownx.snippets at docs/site with an absolute path, so
   the build works from any current directory.
2. on_pre_build: runs the generators (scripts/generate.py): signatures from the
   headers, the decision log page, the coverage report.
3. on_page_markdown: replaces every include that does not resolve (missing
   file or missing section) with a visible placeholder, before pymdownx.snippets
   runs. A missing snippet never breaks the build (docs/DocsProposal.md); it
   shows "Vulkan example not written yet" and is listed in the coverage report.

   Recognised forms: a fenced code block whose only line is the include, and a
   bare include line (e.g. a generated signature). Escaped includes (;--8<--)
   are left alone.
"""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
import docs_common  # noqa: E402
import generate as equine_generate  # noqa: E402

FENCE_OPEN = re.compile(r"^(?P<indent>[ \t]*)(?P<fence>`{3,}|~{3,})")

TITLES = {
    "ri": "RI example not written yet",
    "vulkan": "Vulkan example not written yet",
    "d3d12": "D3D12 example not written yet",
    "signature": "Signature not generated yet",
    "other": "Include not written yet",
}


def on_config(config, **kwargs):
    snippets = config["mdx_configs"].setdefault("pymdownx.snippets", {})
    snippets["base_path"] = [str(docs_common.SITE_DIR)]
    return config


def on_pre_build(config, **kwargs):
    equine_generate.main()


def coverage_link(page) -> str:
    src = Path(page.file.src_uri)
    return Path(os.path.relpath(docs_common.COVERAGE_PAGE.relative_to(docs_common.DOCS_DIR), src.parent or ".")).as_posix()


def placeholder(indent: str, ref: docs_common.SnippetRef, reason: str, link: str) -> list[str]:
    where = f"`{ref.path}`" + (f", section `{ref.section}`" if ref.section else "")
    return [
        f'{indent}!!! warning "{TITLES[ref.kind]}"',
        "",
        f"{indent}    This will come from {where}: {reason}. Missing pieces are listed in the",
        f"{indent}    [coverage report]({link}).",
        "",
    ]


def is_fence_close(line: str, fence: str) -> bool:
    s = line.strip()
    return len(s) >= len(fence) and set(s) == {fence[0]}


def replace_missing(markdown: str, link: str) -> str:
    lines = markdown.split("\n")
    out: list[str] = []
    i = 0
    while i < len(lines):
        line = lines[i]
        m = FENCE_OPEN.match(line)
        if m:
            fence = m.group("fence")
            j = i + 1
            while j < len(lines) and not is_fence_close(lines[j], fence):
                j += 1
            inner = [x for x in lines[i + 1 : j] if x.strip()]
            if len(inner) == 1:
                sm = docs_common.SNIPPET_LINE.match(inner[0])
                if sm and not sm.group("escape"):
                    ref = docs_common.parse_ref(sm.group("ref"))
                    reason = docs_common.missing_reason(ref)
                    if reason:
                        out += placeholder(m.group("indent"), ref, reason, link)
                        i = j + 1
                        continue
            out += lines[i : j + 1]
            i = j + 1
            continue
        sm = docs_common.SNIPPET_LINE.match(line)
        if sm and not sm.group("escape"):
            ref = docs_common.parse_ref(sm.group("ref"))
            reason = docs_common.missing_reason(ref)
            if reason:
                out += placeholder(sm.group("indent"), ref, reason, link)
                i += 1
                continue
        out.append(line)
        i += 1
    return "\n".join(out)


def on_page_markdown(markdown, page, config, files, **kwargs):
    return replace_missing(markdown, coverage_link(page))
