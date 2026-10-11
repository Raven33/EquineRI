# Code documentation standard

What "clear documentation" means for EquineRI's code, defined with the user (2026-10-11/12).
The docs site (`docs/site/`) is for people using RI; this standard is for people and agents
who open the code.

## Readers

Both, **contributors first**: someone (a programmer or an agent, starting cold) who changes RI
itself. Users of RI's API come second: the site already serves them, and the header rules
below cover what they need at the declaration.

## Where it lives

1. **`ARCHITECTURE.md`** at the repository root, the single entry point. It contains:
   - **Layout and call flow:** where everything is (`src/RI`, `backends/`, `helpers/`,
     `tests/`, `docs/`) and how one call travels: the `RI::` alias → the backend function →
     the validation check → the native call.
   - **Invariants:** the rules every change keeps, each with its decision (P..): RI allocates
     nothing itself (P31), infers only what is evident (P36), no OOP (P82), every feature on
     both backends, native access gated (P84), and the others.
   - **Step-by-step recipes:** add a command to both backends, add a validation check, add a
     GPU test, add a docs page, verify on Windows.
   - **Backend differences:** where Vulkan and D3D12 differ underneath RI and how each is
     bridged (layouts, heaps, root data, indirect, clears, ...).
2. **A header comment at the top of every source file:** what the file owns, and which
   decisions (P..) it implements.
3. **Comments on public declarations**, under the rules below.

## Comments on declarations

- **Signatures document themselves first.** Names and types say what a function takes and
  does. A signature is never changed without the user's agreement.
- **A comment only when the signature cannot say it:** expectations on the caller,
  side effects, or a critical impact on the frame (waiting, internal submissions,
  allocations).
- **Short prose, fixed topics.** When a comment is needed, a few plain sentences cover, as
  they apply: what it does beyond the name, what the caller must ensure, what is reported
  (and in which builds), cost or waiting, and the decision (P..). No labels.

## Keeping it true

- **Rule:** every change updates the comments, file headers and `ARCHITECTURE.md` sections it
  touches; each recipe ends with that step.
- **Automatic check**, run with the docs build: it fails when a source file has no header
  comment, or when a comment names a decision (P..) that does not exist in `CONTEXT.md`.

## Acceptance

- **Cold-agent test** during the sample ports: agents with no history port samples using
  only the repository. Where they hesitate, guess wrong or search long, the documentation is
  fixed.
- **The user reviews and judges `ARCHITECTURE.md`.**
