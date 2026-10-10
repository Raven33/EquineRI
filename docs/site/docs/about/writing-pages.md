# Writing pages

How this site is built and how to add to it. A feature is not done until its page exists;
its snippets may follow later.

## Build the site

From the repository root, once:

```text
python3 -m venv .venv
.venv/bin/pip install -r docs/site/requirements.txt
```

Then:

```text
.venv/bin/mkdocs serve -f docs/site/mkdocs.yml          # live preview at http://127.0.0.1:8000
.venv/bin/mkdocs build --strict -f docs/site/mkdocs.yml # the site, in docs/site/site/
```

Every build first runs `docs/site/scripts/generate.py`, which regenerates the signatures from
the headers, the [decision log](../design-notes/decisions.md), and the
[coverage report](coverage.md). You can run it on its own with
`python3 docs/site/scripts/generate.py` (standard library only).

The built site works offline: open `docs/site/site/index.html` in a browser. The version
selector only appears on a site published with `mike` (below).

## Where things are

| Path | What |
|---|---|
| `docs/site/mkdocs.yml` | Site configuration and navigation. |
| `docs/site/docs/` | The pages. `page.md` is English; `page.fr.md` its French translation. |
| `docs/site/templates/reference-page.md` | The template for a reference page. |
| `docs/site/snippets/{ri,vulkan,d3d12}/` | Code examples: complete, compilable source files. |
| `docs/site/scripts/` | Header extraction, decision log, coverage report. |
| `docs/site/hooks/equine_docs.py` | Runs the scripts and shows placeholders for missing snippets. |
| `docs/site/generated/` | Generated includes (signatures, `api.json`). Not committed. |

## Add a reference page

1. Copy `docs/site/templates/reference-page.md` to `docs/site/docs/reference/<name>.md`.
2. List the symbols the page documents in its front matter, e.g.
   `symbols: [RI::createDevice, RI::destroyDevice]`. The coverage report uses this list.
3. Add the page to `nav` in `mkdocs.yml`.
4. Write the prose. Write it for someone who has not used Vulkan or D3D12: plain words,
   one step at a time.
5. Reference the snippets, even before they exist. A missing one shows a placeholder.

## Snippets

A snippet is a **complete source file** (includes, `main`, cleanup) under
`docs/site/snippets/`, in the folder of the backend it is compiled against:

- `ri/`: RI code, compiled once per enabled backend;
- `vulkan/`: canonical Vulkan code, and code using `<RI/Vk/Native.hpp>`;
- `d3d12/`: canonical D3D12 code, and code using `<RI/D3D12/Native.hpp>`.

A page shows only a **named section** of the file, between two marker comments:

```cpp
// ;--8<-- [start:create-device]
RI::Device device = RI::createDevice({.gpu = chosen});
// ;--8<-- [end:create-device]
```

(Without the `;`, which is only there so this page can show the markers.) Section names are
lowercase letters, digits, `-` and `_`. Sections may nest, and the shown lines are
dedented. In a page, include a section like this:

````markdown
```cpp
;--8<-- "snippets/ri/device.cpp:create-device"
```
````

and the native versions in linked tabs, labelled exactly `RI`, `Vulkan`, `D3D12` so that
choosing one switches every tab group on the site:

```markdown
=== "Vulkan"

    ```cpp
    ;--8<-- "snippets/vulkan/device.cpp:create-device"
    ```
```

**The native code is canonical**: how a student would do the same thing in their own
renderer with the native API alone, with notes on what to watch for. It is not a copy of
RI's backend.

### Missing snippets

A missing file or section never breaks the build. The hook replaces the code block with a
visible warning such as *"Vulkan example not written yet"*, and the
[coverage report](coverage.md) lists it. The same applies to a signature include for a
symbol that is not in the headers.

### Compile the snippets

Locally, not in CI (it needs the SDKs and is heavy):

```text
cmake --preset clang-debug -DEQUINE_BUILD_DOC_SNIPPETS=ON
cmake --build --preset clang-debug
```

Every file is compiled as an object file and never run. D3D12 snippets are compiled only
when the D3D12 backend is enabled (Windows, `clangcl-debug`). The usual clang-tidy checks
apply, so every `if` body needs braces.

## Signatures

`scripts/extract_headers.py` reads the public headers (`src/RI/*.hpp`,
`backends/*/RI/*/*.hpp`) and writes one include per symbol into
`docs/site/generated/signatures/`: the declaration, the header comment, and a table of fields
or enumerators. Portable symbols use their `RI::` name (`RI.createDevice.md`); native ones
keep the backend (`RI.Vk.DeviceFeatures.md`):

```markdown
;--8<-- "generated/signatures/RI.createDevice.md"
```

## Decisions

Cite decisions with a link to the generated decision log, relative to the page, e.g. from
`docs/reference/`: `[P60](../design-notes/decisions.md#p60)`. The strict build checks the
anchor, so a decision number that does not exist fails the build; the coverage report also
lists unknown numbers.

## Translations

A translation of `page.md` is `page.fr.md` next to it. Untranslated pages fall back to
English. Navigation titles are translated in `mkdocs.yml` (`nav_translations`). The coverage
report lists stale translations: pages whose English text changed after the translation (by
git history).

## Versions

The version selector reads the versions published with `mike`, each a full copy of the
built site. Until v1, only two are kept: `dev` (the development branch) and `latest` (the
last release). Nothing is published yet; when hosting is chosen, publishing looks like:

```text
.venv/bin/mike deploy -F docs/site/mkdocs.yml dev
.venv/bin/mike deploy -F docs/site/mkdocs.yml --update-aliases 0.1 latest
.venv/bin/mike set-default -F docs/site/mkdocs.yml latest
```
