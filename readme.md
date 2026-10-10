# EquineRI

Research rendering interface over Direct3D 12 and Vulkan, with HLSL as the shading
language. Bindless first. Successor to EquineEngineV2.

Status: design finished, implementation starting. See `CONTEXT.md` for the decisions,
`docs/Plan.md` for the implementation plan, `docs/RIApiProposal.md` for the API style,
and `docs/GrillLog.md` for the discussion record. The build currently contains only the
scaffold (integer aliases, backend header-check targets, an empty GPU-free test harness).

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
