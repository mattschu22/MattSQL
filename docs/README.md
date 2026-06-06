# MattSQL Docs

Start here when navigating project documentation.

- [Overview](overview.md): project purpose, boundaries, source layout, and build
  commands.
- [Dev container](../.devcontainer/README.md): Ubuntu 24.04 development setup
  for macOS-hosted editing.
- [Current state](status/current_state.md): implemented pipeline, storage/runtime
  status, tests, and known gaps.
- [Code-level invariants](reference/invariants.md): well-formedness rules for
  objects crossing module boundaries.
- [Runtime C ABI](reference/runtime_abi.md): version-1 ABI contract for runtime
  backends.
- [Rust kernel runtime strategy](reference/kernel_runtime_strategy.md):
  architecture, allocator, and ABI-consumption decisions for the first Rust
  kernel/runtime slice.
- [Long-term plan](../plan.txt): broader OS/DBMS roadmap and research
  direction.

Historical cleanup reports are intentionally not kept as living docs. Durable
module contracts belong in the invariants and ABI references; implementation
status belongs in the current-state page.
