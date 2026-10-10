# EquineRI

Research rendering interface over Direct3D 12 and Vulkan, with HLSL as the shading
language. Bindless first. Successor to EquineEngineV2.

Status: design finished, implementation starting. See `CONTEXT.md` for the decisions,
`docs/Plan.md` for the implementation plan, `docs/RIApiProposal.md` for the API style,
and `docs/GrillLog.md` for the discussion record. The build currently contains only the
scaffold (shared vocabulary types, backend header-check targets, GPU-free tests).

```text
git submodule update --init --recursive
cmake --preset clangcl-debug     # Windows: both backends
cmake --preset clang-debug       # Linux: Vulkan
cmake --build --preset <preset>
ctest --preset <preset>
```
