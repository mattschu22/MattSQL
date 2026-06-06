# Rust Kernel Runtime Strategy

This note records the decisions that unblock the first Rust kernel/runtime
implementation while keeping the runtime ABI narrow and low overhead.

Related docs:

- [Docs index](../README.md)
- [Current state](../status/current_state.md)
- [Runtime C ABI](runtime_abi.md)
- [Code-level invariants](invariants.md)

## Decision Summary

The first Rust work targets an x86_64 QEMU vertical slice, with the code shaped
so AArch64 can be added after the initial runtime path works.

The version-1 C ABI is frozen. Rust code should consume the existing
`mattsql_abi_runtime_v1` table and must not require new callbacks, new fields,
or changed status meanings. Any expansion after this point requires a new ABI
version.

The Rust provider smoke test is a hosted `staticlib` linked into a small C++
CTest executable. This proves that Rust can export a v1 runtime table and that
the C++ consumer adapter can drive it through the real ABI.

The kernel allocation model is a physical page-frame span allocator. Higher
level heaps, slabs, and arenas may be built above it later, but the ABI path
deals in explicit page spans and exact free metadata.

The kernel runtime implementation should use concrete modules and structs.
Avoid broad trait layers on hot runtime paths until there are at least two real
implementations that need the abstraction. The C ABI function table is the
external dispatch boundary.

## Alternatives And Tradeoffs

ABI freeze alternatives:

- Documentation-only freeze has almost no overhead, but it relies on discipline
  and does not prove cross-language consumption.
- Documentation plus C++ layout tests, Rust layout tests, and a Rust provider
  smoke test catches practical drift while staying dependency-light.
- Generated bindings and ABI hashes provide stronger mechanical guarantees, but
  add tools and build complexity before the kernel exists.

Chosen: documentation plus layout and smoke tests.

Rust provider smoke alternatives:

- A Rust-only test is fast and simple, but it does not prove the C++ adapter can
  consume the Rust table.
- A Rust `staticlib` linked into C++ proves the real FFI path with minimal build
  machinery.
- A Cargo workspace is the likely long-term shape, but it is more structure than
  the first ABI smoke needs.

Chosen: Rust `staticlib` plus C++ CTest executable.

Allocator alternatives:

- A global allocator-first design is ergonomic, but it hides the physical-frame
  model and introduces abstraction before hardware ownership is clear.
- A page-frame span allocator matches the hardware and the ABI directly. It has
  low overhead and keeps ownership explicit, at the cost of less convenience for
  general data structures.
- A full buddy/slab hierarchy is useful later, but premature for bootstrapping
  the runtime boundary.

Chosen: page-frame span allocator first.

Runtime abstraction alternatives:

- Broad runtime traits improve mockability, but can turn the kernel into a
  stack of indirections before the architecture boundary is known.
- Concrete modules and structs keep code direct and cheap. Tests can exercise
  the ABI table and public module functions.
- Macro-generated backend layers are flexible, but make the first kernel harder
  to inspect and debug.

Chosen: concrete modules and structs, with the C ABI table as the polymorphic
boundary.

## Allocation Model

The kernel owns physical memory. Boot code provides a memory map with usable
physical ranges. The memory subsystem normalizes those ranges into page frames
using the runtime page size, initially 4096 bytes on x86_64.

The minimum allocator is:

- a physical frame range iterator over bootloader-provided usable memory
- a contiguous frame-span allocator for `page_count` requests
- a small free-list path for exact spans returned through `free_pages`
- explicit zeroing when `MATTSQL_ABI_MEMORY_ZEROED` is requested
- rejection of `MATTSQL_ABI_MEMORY_DMA` until the device/DMA model is present

`allocate_pages(page_count, alignment, flags, out_allocation)` maps to a
contiguous physical frame span. The returned pointer is the kernel virtual
address that C++ may access. `physical_address` remains zero until DMA support
is implemented and advertised.

`free_pages(context, allocation)` must receive the exact allocation metadata
returned by `allocate_pages`. The first implementation may validate exact
metadata through a compact allocation header or side table. It should not trust
caller-fabricated spans.

The ABI allocation path should not require a Rust global allocator, heap boxes,
trait objects, or dynamically allocated callback state. If a later heap is
needed for Rust internals, it should be built on top of the frame allocator and
kept out of the ABI fast path.

## First Kernel Milestones

1. Build and run the hosted Rust ABI provider smoke test.
2. Add a freestanding Rust crate with architecture-neutral ABI definitions.
3. Boot x86_64 in QEMU and write serial logs.
4. Install panic handling that does not unwind across FFI.
5. Parse boot memory ranges and allocate physical frame spans.
6. Export a `mattsql_abi_runtime_v1` table backed by the kernel runtime.
7. Add virtio-blk after allocation, logging, panic, and monotonic time work.
