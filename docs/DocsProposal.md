# User-facing documentation: proposal (2026-10-10)

Status: **decided 2026-10-10** (user: "ok with me"): MkDocs + Material as a test (we may roll our own later), the page shape, the site sections, and the sync plan below. Remaining open: reference prose hand-written vs generated beyond signatures, hosting, which human languages.

## Goal (user)

A comprehensive website documenting **all** of RI (later also EquineEngine, kept separate
per P80). Whenever RI wraps an underlying behavior, the page shows explicit examples of
what it would have looked like in each backend (Vulkan, D3D12). Scaffold it early, so
each feature is documented as it is implemented.

## Requirements (user, 2026-10-10)

- Site features: **language toggle**, **theme toggle** (light/dark), **API version
  selector**, **offline mode**.
- The site is **also a teaching resource**: a student who learned on EquineRI (and later
  EquineEngine) and wants to write their own Vulkan or D3D12 toy renderer opens the page
  for an RI feature and finds how to achieve the same thing in each native API.
- So the Vulkan/D3D12 code shown is a **canonical native version** of the feature, not
  necessarily what RI's backend does internally. (This does not change §1 of CONTEXT.md:
  RI is not designed for teaching; the documentation teaches.)

## Proposed page shape: one page per RI function or concept

```
RI::cmdBeginRendering
  What it does        — plain explanation
  Signature           — from the header
  Example             — RI code (a real, compiled snippet)
  The same in native code
    [Vulkan] [D3D12]  — tabs: a canonical native version (compiled snippet), with notes on
                        what to watch for (sync, layouts, alignment, …)
  Rules / errors      — validation checks (P88), capability needs (P11)
  Related decisions   — P-numbers (link to CONTEXT.md)
```

## Proposed site sections

1. Getting started (build, first compute dispatch, first triangle).
2. Concepts (bindless, timeline points, memory kinds, layouts hidden by A', native
   sections, the inference rule).
3. Reference (every public function and struct, the page shape above).
4. Native access (P84): per-backend pages.
5. Helpers (P41), each with "how to roll your own".
6. Design notes (generated from CONTEXT.md).

## Keeping the site in sync (proposal, revised after user feedback)

User constraints: a missing snippet must never stop the build (snippets may be missing or
take time to write); compiling snippets is very heavy and cannot run in a free GitHub
Action; pages should show only the relevant part of a snippet, for readability.

1. **Snippets are full, compilable files; pages show only a marked section.** Each snippet
   file under `docs/site/snippets/{ri,vulkan,d3d12}/` is complete code (setup, includes),
   but the page includes only the lines between named markers (pymdownx.snippets
   sections), optionally with a "full example" link to the whole file.
2. **Missing never blocks.** A missing snippet or page shows a visible placeholder ("Vulkan
   example not written yet") and appears in a **coverage report** (a generated page listing
   what is missing). Nothing in the docs pipeline fails the build.
3. **Compilation is local, not CI.** A CMake target (`EQUINE_BUILD_DOC_SNIPPETS`) compiles
   all snippets; the user runs it on their machine (it needs the Vulkan SDK, the Windows
   SDK, the Agility SDK, and a GPU to run them). The free CI job only builds the site and
   the coverage report (Python, seconds).
4. **Generated signatures and API list** from RI's portable headers with clang (cheap:
   portable headers contain no native types, P84). Feeds the signature blocks and the
   coverage report (public symbol without a page, page for a symbol that no longer exists).
5. **Decision links** (P-numbers) and broken links are listed in the same report.
6. **Translations** fall back to English; stale translations are listed in the report.
7. **Definition of done:** a feature is not done until its page exists (snippets may follow
   later).
8. **Versions:** see below.

## What a "version" of the site is

With mike (the versioning tool for MkDocs + Material), each published version is a **full
built copy** of the static site in its own folder (`/0.1/`, `/0.2/`, …, plus aliases such
as `/latest/`) on the hosting branch. The version selector switches between folders. Old
versions are never rebuilt. A copy is small (static HTML, a few MB). Options: keep every
release, or only `latest` + `dev` until v1 ships.

## Choices to make, one at a time

1. **Site tool.** Requirement fit:

   | | Language toggle | Theme toggle | Version selector | Offline | Synced code tabs |
   |---|---|---|---|---|---|
   | MkDocs + Material | plugin (mkdocs-static-i18n) | built in | built in (mike) | built in (`offline` plugin, works from `file://`) | built in (linked tabs) |
   | Docusaurus | built in | built in | built in | plugin (PWA) | built in (`groupId`) |
   | Sphinx (Furo) | gettext | theme | sphinx-multiversion | static HTML | extension |

   Note: Material for MkDocs is in maintenance mode; its authors' successor (Zensical)
   reads the same configuration and Markdown, so content stays portable.

   Candidates:
   - MkDocs + Material theme (recommended): Markdown pages, built-in content tabs
     (`RI | Vulkan | D3D12`), search, snippet inclusion from source files, versioning
     (mike). Python tool.
   - Docusaurus: React/MDX, tabs, versioning. Node tool; heavier.
   - Sphinx (+ Breathe for C++): strong cross-referencing, reStructuredText by default.
   - Doxygen alone: generated from comments; weak for long-form pages and tabs.
   - mdBook: simple, no tabs without plugins.
2. **Reference source of truth:** hand-written Markdown per function, generated from
   header comments (Doxygen XML → pages), or a mix (signature generated, prose written).
3. **Keeping examples true:** every snippet (RI and native) lives in a source file that is
   compiled in CI, and the page includes it by marker, so examples cannot rot.
4. **Coverage check:** a script fails the build when a public RI function has no page
   (same idea as the conformance file, P83).
5. **Location and hosting:** a `docs/site/` folder in this repo vs a separate repo;
   GitHub Pages (needs a paid plan for a private repo) or another host.
6. **When pages are written:** as part of each milestone's "done" (a feature is not done
   until its page exists).
