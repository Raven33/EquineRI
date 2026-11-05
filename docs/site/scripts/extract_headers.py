#!/usr/bin/env python3
"""Extracts RI's public API from its headers, for the Reference pages.

Reads the portable headers (src/RI/*.hpp) and each backend's public headers
(backends/*/RI/*/*.hpp) and writes:

  generated/api.json                   every public symbol (the coverage report reads it)
  generated/signatures/<Symbol>.md     one Markdown include per symbol, e.g.
                                       RI.listGpus.md, RI.Gpu.md, RI.Vk.DeviceFeatures.md

Pages include a signature with:  --8<-- "generated/signatures/RI.listGpus.md"

Naming (P83, P84, P90): a symbol in a backend's Backend.hpp is portable and is
listed under its RI:: name (RI::Vk::listGpus and RI::D3D12::listGpus are both
RI::listGpus). A symbol in a backend's Native.hpp keeps its backend name
(RI::Vk::defaultDeviceFeatures). Members of a nested `internal` struct are
backend-private and left out (P82).

The parser is deliberately small (standard library only) and handles the shapes
RI's headers use: namespaces, structs with fields, enum classes, type aliases
and free-function declarations, with `//` comments. The proposal's clang-based
extraction can replace it if the headers outgrow it.
"""

from __future__ import annotations

import json
import re
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from docs_common import API_JSON, REPO_DIR, SIGNATURES_DIR, symbol_file_name, write_if_changed  # noqa: E402

BACKEND_NAMES = {"Vk": "vulkan", "D3D12": "d3d12"}
BACKEND_LABELS = {"common": "Portable", "vulkan": "Vulkan", "d3d12": "D3D12"}


@dataclass
class Field:
    declaration: str  # e.g. "GpuVendor vendor = GpuVendor::eOther"
    name: str
    type: str
    default: str
    comment: str
    condition: str = ""  # preprocessor condition, if any


@dataclass
class Decl:
    kind: str  # function | struct | enum | alias
    name: str  # short name
    namespace: str  # e.g. RI::Vk
    header: str  # repository-relative path
    include: str  # what user code includes, e.g. <RI/Vk/Native.hpp>
    backend: str  # common | vulkan | d3d12
    native: bool
    comment: str
    code: str  # the declaration as written (comments inside kept)
    fields: list[Field] = field(default_factory=list)

    @property
    def symbol(self) -> str:
        if self.backend != "common" and not self.native:
            return f"RI::{self.name}"  # portable: reached through the RI:: alias
        return f"{self.namespace}::{self.name}"


def public_headers() -> list[Path]:
    headers = sorted((REPO_DIR / "src" / "RI").glob("*.hpp"))
    headers += sorted(REPO_DIR.glob("backends/*/RI/*/*.hpp"))
    return headers


def header_info(path: Path) -> tuple[str, str, bool]:
    parts = path.relative_to(REPO_DIR).parts
    if parts[0] == "backends":
        include = "<" + "/".join(parts[2:]) + ">"
        backend = BACKEND_NAMES.get(parts[3], parts[1])
        native = path.stem == "Native"
        return include if native else "<RI/RI.hpp>", backend, native
    return "<RI/RI.hpp>", "common", False


def strip_comment(line: str) -> tuple[str, str]:
    """Splits `code // comment` (no string literals in RI's declarations)."""
    idx = line.find("//")
    if idx < 0:
        return line, ""
    return line[:idx], line[idx + 2 :].strip()


def parse_field(code: str, comment: str, condition: str) -> Field | None:
    code = code.strip().rstrip(";").strip()
    if not code:
        return None
    default = ""
    m = re.match(r"^(?P<decl>.*?)\s*(?:=\s*(?P<eq>.+)|(?P<brace>\{.*\}))$", code)
    decl = code
    if m and (m.group("eq") or m.group("brace")):
        decl = m.group("decl")
        default = (m.group("eq") or m.group("brace") or "").strip()
    fp = re.match(r"^(?P<ret>.+?)\(\s*\*\s*(?P<name>\w+)\s*\)\s*(?P<params>\(.*\))$", decl.strip())
    if fp:  # a function pointer: `void (*name)(args)`
        return Field(code, fp.group("name"), f"{fp.group('ret').strip()} (*)" + fp.group("params"), default, comment, condition)
    nm = re.match(r"^(?P<type>.+?[\s*&])(?P<name>\w+)$", decl.strip())
    if not nm:
        return None
    return Field(code, nm.group("name"), nm.group("type").strip(), default, comment, condition)


def parse_header(path: Path) -> list[Decl]:
    include, backend, native = header_info(path)
    rel = path.relative_to(REPO_DIR).as_posix()
    lines = path.read_text(encoding="utf-8").splitlines()
    decls: list[Decl] = []
    namespaces: list[list[str]] = []
    comment: list[str] = []
    in_file_comment = True  # the comment at the top describes the file, not a symbol
    i = 0

    def ns() -> str:
        return "::".join(part for group in namespaces for part in group)

    while i < len(lines):
        raw = lines[i]
        s = raw.strip()
        if not s:
            comment = []
            i += 1
            continue
        if s.startswith("//"):
            if not in_file_comment:
                comment.append(s[2:].strip())
            i += 1
            continue
        in_file_comment = False
        if s.startswith("#"):
            while lines[i].rstrip().endswith("\\"):
                i += 1
            comment = []
            i += 1
            continue
        m = re.match(r"^namespace\s+([\w:\s]+?)\s*\{", s)
        if m:
            # RI::inline EQUINE_RI_ABI::Vk is RI::Vk to users (P110).
            parts = [part.strip() for part in m.group(1).split("::")]
            namespaces.append([part for part in parts if not part.startswith("inline ")])
            comment = []
            i += 1
            continue
        if s.startswith("}"):
            if namespaces:
                namespaces.pop()
            comment = []
            i += 1
            continue
        if re.match(r"^(struct|class)\s+\w+\s*;", s) or s.startswith("using namespace"):
            comment = []
            i += 1
            continue

        m = re.match(r"^struct\s+(\w+)\s*\{", s)
        if m:
            i, decl = parse_struct(lines, i, m.group(1))
            decls.append(Decl("struct", m.group(1), ns(), rel, include, backend, native, " ".join(comment), decl[0], decl[1]))
            comment = []
            continue

        m = re.match(r"^enum\s+class\s+(\w+)", s)
        if m:
            i, decl = parse_enum(lines, i)
            decls.append(Decl("enum", m.group(1), ns(), rel, include, backend, native, " ".join(comment), decl[0], decl[1]))
            comment = []
            continue

        m = re.match(r"^using\s+(\w+)\s*=\s*(.+);", s)
        if m:
            decls.append(Decl("alias", m.group(1), ns(), rel, include, backend, native, " ".join(comment), s))
            comment = []
            i += 1
            continue

        # A macro invocation without a semicolon, e.g. makeBitmask(Stage).
        if re.match(r"^\w+\([\w:, ]*\)$", s):
            comment = []
            i += 1
            continue

        # A statement: a declaration (possibly over several lines), or an
        # inline function with its body. Bodies are dropped from the signature.
        start = i
        statement: list[str] = []
        depth = 0
        while i < len(lines):
            code = strip_comment(lines[i])[0]
            statement.append(code.strip())
            depth += code.count("{") - code.count("}")
            stripped = code.rstrip()
            if depth <= 0 and (stripped.endswith(";") or (stripped.endswith("}") and "{" in " ".join(statement))):
                break
            i += 1
        i += 1
        text = " ".join(part for part in statement if part)
        template = ""
        m = re.match(r"^(template\s*<[^>]*>)\s*(.*)$", text)
        if m:
            template, text = m.group(1), m.group(2)
        head = text
        fm = re.match(
            r"^(?P<ret>(?:\[\[[^\]]*\]\]\s*)?(?:inline\s+)?[\w:<>,&*\s]+?[\s*&])(?P<name>\w+)\s*\((?P<params>(?:[^{]|\{\})*?)\)\s*(?:;|\{.*\})$",
            text,
        )
        if fm:
            head = f"{fm.group('ret').replace('inline ', '').strip()} {fm.group('name')}({fm.group('params').strip()});"
            head = re.sub(r"\s+([*&])\s*(?=\w)", r" \1", head)
            if "{" not in text:
                # Keep the header's own line breaks for plain declarations.
                head = dedent("\n".join(line.rstrip() for line in lines[start:i]))
            code = (template + "\n" if template else "") + head
            decls.append(Decl("function", fm.group("name"), ns(), rel, include, backend, native, " ".join(comment), code))
        else:
            cm = re.match(r"^(?:inline\s+)?(?:constexpr\s+)?(?:const\s+)?[\w:<>]+\s+(?P<name>\w+)\s*=.*;$", text)
            if cm:
                decls.append(Decl("constant", cm.group("name"), ns(), rel, include, backend, native, " ".join(comment), text))
        comment = []
    return decls


def dedent(code: str) -> str:
    lines = code.splitlines()
    first_indent = len(lines[0]) - len(lines[0].lstrip())
    return "\n".join(line[first_indent:] if line[:first_indent].isspace() else line for line in lines)


def parse_struct(lines: list[str], i: int, name: str) -> tuple[int, tuple[str, list[Field]]]:
    """Returns the index after `};`, the code (internal state elided) and the fields."""
    code = [lines[i].strip()]
    fields: list[Field] = []
    pending: list[str] = []
    condition: list[str] = []
    i += 1
    while i < len(lines):
        raw = lines[i].rstrip()
        s = raw.strip()
        if s.startswith("};"):
            code.append("};")
            return i + 1, ("\n".join(code), fields)
        if s.startswith("struct {") or s == "struct {":
            # Backend-private state (P82): skip to the closing `} internal;`.
            depth = 0
            while i < len(lines):
                depth += lines[i].count("{") - lines[i].count("}")
                if depth <= 0 and "}" in lines[i]:
                    break
                i += 1
            code.append("    // internal: backend-private state (not part of the API)")
            pending = []
            i += 1
            continue
        code.append(raw)
        if not s:
            pending = []
        elif s.startswith("//"):
            pending.append(s[2:].strip())
        elif s.startswith("#if"):
            condition.append(s)
        elif s.startswith("#endif"):
            if condition:
                condition.pop()
        elif not s.startswith("#"):
            body, trailing = strip_comment(s)
            text = " ".join(pending + ([trailing] if trailing else []))
            parsed = parse_field(body, text, " && ".join(condition))
            if parsed:
                fields.append(parsed)
            pending = []
        i += 1
    raise ValueError(f"struct {name}: no closing '}};'")


def parse_enum(lines: list[str], i: int) -> tuple[int, tuple[str, list[Field]]]:
    code = [lines[i].strip()]
    values: list[Field] = []
    one_line = re.match(r"^[^{]*\{(?P<body>[^}]*)\}\s*;", strip_comment(lines[i])[0].strip())
    if one_line:  # `enum class LoadOp : uint8 { eLoad, eClear, eDontCare };`
        for body in one_line.group("body").split(","):
            body = body.strip()
            if body:
                name, _, value = body.partition("=")
                values.append(Field(body, name.strip(), "", value.strip(), ""))
        return i + 1, ("\n".join(code), values)
    i += 1
    while i < len(lines):
        raw = lines[i].rstrip()
        s = raw.strip()
        code.append(raw)
        if s.startswith("};"):
            return i + 1, ("\n".join(code), values)
        body, trailing = strip_comment(s)
        body = body.strip().rstrip(",")
        if body:
            name, _, value = body.partition("=")
            values.append(Field(body, name.strip(), "", value.strip(), trailing))
        i += 1
    raise ValueError("enum: no closing '};'")


def strip_code_comments(code: str) -> str:
    return "\n".join(strip_comment(line)[0].rstrip() for line in code.splitlines() if strip_comment(line)[0].strip())


def render(symbol: str, decls: list[Decl]) -> str:
    """One Markdown include for a symbol (all overloads and backends)."""
    first = decls[0]
    namespace = "RI" if symbol.count("::") == 1 else first.namespace
    out = [f"<!-- Generated by docs/site/scripts/extract_headers.py from {', '.join(sorted({d.header for d in decls}))}. -->", ""]

    by_backend: dict[str, list[Decl]] = {}
    for d in decls:
        by_backend.setdefault(d.backend, []).append(d)
    same_code = len({"\n\n".join(strip_code_comments(d.code) for d in group) for group in by_backend.values()}) == 1

    def block(group: list[Decl]) -> list[str]:
        lines = ["```cpp", f"// {group[0].include}, namespace {namespace}"]
        for d in group:
            lines.append(d.code)
        lines.append("```")
        return lines

    if same_code:
        out += block(by_backend[next(iter(by_backend))])
    else:
        for backend, group in by_backend.items():
            out.append(f'=== "{BACKEND_LABELS[backend]}"')
            out.append("")
            out += ["    " + line if line else "" for line in block(group)]
            out.append("")

    # Header comments: shown per backend when they differ.
    notes = {(d.backend, d.comment) for d in decls if d.comment}
    if notes:
        out.append("")
        if len({c for _, c in notes}) == 1:
            out.append(f"*From the header:* {next(iter(notes))[1]}")
        else:
            for backend, text in sorted(notes):
                out.append(f"- *{BACKEND_LABELS[backend]} header:* {text}")

    fields = first.fields
    if first.kind == "struct" and fields:
        out += ["", "| Field | Type | Default | Description |", "|---|---|---|---|"]
        for f in fields:
            desc = f.comment + (f" (only when {describe_condition(f.condition)})" if f.condition else "")
            default = f"`{f.default}`" if f.default else ""
            out.append(f"| `{f.name}` | `{f.type}` | {default} | {escape_cell(desc)} |")
    elif first.kind == "enum" and fields:
        out += ["", "| Value | Meaning |", "|---|---|"]
        for f in fields:
            out.append(f"| `{f.name}` | {escape_cell(f.comment)} |")
    return "\n".join(out).rstrip() + "\n"


def describe_condition(condition: str) -> str:
    parts = []
    for c in condition.split(" && "):
        m = re.match(r"^#\s*if(n?)def\s+(\w+)$", c)
        if m:
            parts.append(f"`{m.group(2)}` is {'not ' if m.group(1) else ''}defined")
        else:
            expression = re.sub(r"^#\s*if\s+", "", c)
            parts.append(f"`{expression}`")
    return " and ".join(parts)


def escape_cell(text: str) -> str:
    return text.replace("|", "\\|")


def extract() -> dict[str, list[Decl]]:
    symbols: dict[str, list[Decl]] = {}
    for header in public_headers():
        for decl in parse_header(header):
            symbols.setdefault(decl.symbol, []).append(decl)
    return dict(sorted(symbols.items()))


def main() -> int:
    symbols = extract()
    SIGNATURES_DIR.mkdir(parents=True, exist_ok=True)
    wanted = set()
    for symbol, decls in symbols.items():
        name = symbol_file_name(symbol)
        wanted.add(name)
        write_if_changed(SIGNATURES_DIR / name, render(symbol, decls))
    for stale in SIGNATURES_DIR.glob("*.md"):
        if stale.name not in wanted:
            stale.unlink()
    api = {
        symbol: {
            "kind": decls[0].kind,
            "native": decls[0].native,
            "backends": sorted({d.backend for d in decls}),
            "headers": sorted({d.header for d in decls}),
            "include": decls[0].include,
            "signature": f"generated/signatures/{symbol_file_name(symbol)}",
            "declarations": [asdict(d) for d in decls],
        }
        for symbol, decls in symbols.items()
    }
    write_if_changed(API_JSON, json.dumps(api, indent=2) + "\n")
    print(f"extract_headers: {len(symbols)} public symbols -> {SIGNATURES_DIR.relative_to(REPO_DIR)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
