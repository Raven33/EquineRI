# EquineRI

Research rendering interface over Direct3D 12 and Vulkan, with HLSL as the shading
language. Bindless first. Successor to EquineEngineV2.

Status: v1 implemented on both backends (`docs/Plan.md`). Start with `ARCHITECTURE.md` (layout,
call flow, rules, recipes), then `CONTEXT.md` for the decisions. `HANDOFF.md` gives the
current state. The docs site has a [Vulkan and D3D12 to RI](docs/site/docs/concepts/glossary.md)
conversion chart.

```text
cmake --preset clangcl-debug     # Windows: both backends
cmake --preset clang-debug       # Linux: Vulkan
cmake --build --preset <preset>
ctest --preset <preset>
```

Documentation site (MkDocs + Material, `docs/site/`; see `docs/DocsProposal.md`):

```text
python3 -m venv .venv && .venv/bin/pip install -r docs/site/requirements.txt
.venv/bin/mkdocs build --strict -f docs/site/mkdocs.yml   # output: docs/site/site/
cmake --preset clang-debug -DEQUINE_BUILD_DOC_SNIPPETS=ON  # also compile the code snippets
```
