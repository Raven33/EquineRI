# Design notes

EquineRI was designed before it was written. Every decision has a number (P1 to P93 so far)
and is recorded with its reason in the repository's `CONTEXT.md`. Pages on this site cite
the decisions behind each function under **Related decisions**.

- **[Decision log](decisions.md)**: every settled decision, the feature scope, and the
  questions still open. Generated from `CONTEXT.md` on every build, so it is never out of
  date.

## The posture in a few lines

- RI sits **above** Vulkan and D3D12, but hides nothing that researchers need to build
  complex systems ([P1](decisions.md#p1)).
- The base makes **as few architecture decisions** for you as possible. Frame structure,
  lifetime policy, synchronization strategy belong to you; anything opinionated is a helper
  you can replace ([P15](decisions.md#p15)).
- RI is designed for itself, not for the engine that will be built on it
  ([P80](decisions.md#p80)). It is not designed for teaching; this site is what teaches.
- Choices that may cost performance are **measured first** ([P18](decisions.md#p18)).

The discussion that led to the decisions, the API style proposal, and the implementation
plan are in the repository's `docs/` folder (`GrillLog.md`, `RIApiProposal.md`, `Plan.md`).
They are working documents and are not part of this site.
