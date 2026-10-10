---
# The public symbols this page documents (coverage report). Use the RI:: name
# for portable symbols and the backend name for native ones (RI::Vk::...).
symbols: [RI::functionName]
---

<!--
Page template (docs/DocsProposal.md, "Proposed page shape"). Copy this file to
docs/site/docs/reference/<name>.md, add the page to `nav` in mkdocs.yml, and
replace every <...>. Paths in includes are relative to docs/site. Includes that
do not exist yet are fine: the page shows "... example not written yet" and the
coverage report lists them. Links to decisions: from docs/reference/, use
../design-notes/decisions.md#p60.
-->

# RI::functionName

## What it does

<Plain explanation, step by step, for someone who has not used Vulkan or D3D12:
what the function is for, what it returns, when to call it.>

## Signature

--8<-- "generated/signatures/RI.functionName.md"

## Example

```cpp
--8<-- "snippets/ri/<topic>.cpp:<section>"
```

## The same in native code

The Vulkan and D3D12 code is a *canonical* native version: how you would do the
same thing in your own renderer, not necessarily what RI does internally.

=== "RI"

    ```cpp
    --8<-- "snippets/ri/<topic>.cpp:<section>"
    ```

=== "Vulkan"

    ```cpp
    --8<-- "snippets/vulkan/<topic>.cpp:<section>"
    ```

    What to watch for: <synchronization, layouts, alignment, ...>

=== "D3D12"

    ```cpp
    --8<-- "snippets/d3d12/<topic>.cpp:<section>"
    ```

    What to watch for: <...>

## Rules and errors

- <What RI checks in validation builds (P88), and the error category reported.>
- <Capability needs (P11): what happens on a GPU without the feature.>

## Related decisions

- [P60](../design-notes/decisions.md#p60): <one line on why it matters here>
