# Helpers

RI exposes the explicit, controllable form of every mechanism. The automatic form (picking
a GPU, allocating memory for you, assigning descriptor slots, frames in flight, a profiler, a
frame graph, ...) is a **helper**: optional code built only on RI's public functions
([P41](../design-notes/decisions.md#p41), [P15](../design-notes/decisions.md#p15)).

Because a helper uses nothing private, you can always replace it with your own. Each helper
page will therefore end with **"How to roll your own"**: the RI calls the helper makes, so you
can write a version that fits your project.

!!! note "No helpers yet"
    Helpers come after the core (Milestone 4 of the plan). Until then, see the reference
    pages: for example, choosing a GPU yourself is three lines on
    [`RI::listGpus`](../reference/listGpus.md).
