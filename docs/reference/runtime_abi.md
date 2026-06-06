# Runtime C ABI Contract

This document defines the contract for `MATTSQL_ABI_RUNTIME_VERSION == 1` in
[`include/mattsql/abi/runtime.h`](../../include/mattsql/abi/runtime.h).

Related docs:

- [Docs index](../README.md)
- [Overview](../overview.md)
- [Current state](../status/current_state.md)
- [Code-level invariants](invariants.md)

The C ABI is the boundary between the C++ database engine and a runtime backend
implemented by hosted code, simulation code, or a future Rust kernel. It mirrors
only the MVP runtime surface:

- capabilities
- page-span allocation
- batch-shaped block I/O submission and completion polling
- cooperative yield
- monotonic time
- logging
- panic

It intentionally excludes SQL, task groups, scheduler policy, timers, files,
networking, and filesystem concepts.

## General Rules

All ABI calls use C-compatible structs, fixed-width integers, pointer-plus-length
buffers, opaque `void *context`, and explicit status codes.

The ABI must not expose or require:

- C++ classes, templates, exceptions, RTTI, STL containers, or virtual calls
- Rust-specific types, panics, references, slices, traits, or allocation APIs
- NUL-terminated strings unless a callback explicitly treats a buffer that way

Every callback must return normally except `panic`, which is permitted not to
return. C++ exceptions and Rust panics must not cross the ABI. A backend must
catch or abort before unwinding through the ABI boundary.

Reserved fields must be initialized to zero by callers and ignored by callees.
Future ABI versions may assign meaning to reserved fields.

## Versioning

`mattsql_abi_runtime_v1.version` must equal `MATTSQL_ABI_RUNTIME_VERSION`.

A consumer must reject a runtime table with an unsupported version. A provider
must not change the meaning, size, or ordering of existing version-1 fields.
Breaking changes require a new ABI version and a new top-level runtime table
type.

Function pointers that are required for version 1:

- `get_capabilities`
- `allocate_pages`
- `free_pages`
- `submit_io_batch`
- `poll_io_completions`
- `monotonic_nanos`
- `log`
- `panic`

`yield` is optional. If absent, yielding is a no-op from the C++ adapter's
perspective.

## ABI Lifecycle

Version 1 is kernel-ready but MVP-scoped. It is intended to support the first
Rust kernel runtime boundary without committing SQL, scheduler, filesystem, or
network APIs to the C ABI.

The version-1 working set is frozen as of June 6, 2026. The frozen surface is:

- `include/mattsql/abi/runtime.h`
- the C++ consumer adapter from `mattsql_abi_runtime_v1` to `PlatformRuntime`
- the C++ provider adapter from `PlatformRuntime` to `mattsql_abi_runtime_v1`
- the Rust `repr(C)` mirror used by layout and provider-smoke tests
- the callback set listed in this document

Once Rust kernel code consumes a versioned ABI table, that table is frozen:

- Existing field order, field size, alignment, callback signatures, constant
  values, and status meanings must not change.
- Reinterpreting an existing field is a breaking change, even if the C layout is
  unchanged.
- Breaking changes require a new `MATTSQL_ABI_RUNTIME_VERSION` value and a new
  top-level runtime table type.
- Additive changes may use reserved fields only when old consumers and old
  providers can safely ignore the new meaning. Otherwise, add a new version.

Each supported ABI version must carry the full boundary surface:

- C header definitions.
- C++ consumer adapter from ABI table to `PlatformRuntime`.
- C++ provider adapter from `PlatformRuntime` to ABI table.
- C++ layout tests for every ABI struct.
- Rust `repr(C)` mirror and layout test.
- Contract documentation for ownership, validation, and callback semantics.

Version 1 has no negotiation beyond exact matching:
`mattsql_abi_runtime_v1.version` must equal `MATTSQL_ABI_RUNTIME_VERSION`.
Consumers must reject any other version. Providers must export the version they
actually implement.

Older adapters should remain available until no supported kernel or provider
depends on them. Any ABI change must update the C header, adapters, C++ tests,
Rust layout mirror, and this document in the same patch.

After the version-1 working set freeze, edits to version 1 are limited to
contract-preserving bug fixes, additional negative tests, and documentation that
clarifies existing behavior. New callbacks, new fields, changed status meanings,
and changed ownership rules require a new ABI version.

## Status Values

`mattsql_abi_status.code == MATTSQL_ABI_STATUS_OK` indicates success. Any other
code indicates failure.

`message` is optional diagnostic text. It is a borrowed pointer valid until the
callback returns. Consumers that need to retain it must copy
`message_length` bytes immediately.

`message` is not required to be NUL-terminated. If `message == NULL`,
`message_length` must be ignored and treated as zero.

## Capabilities

`get_capabilities(context, out_capabilities)` reports runtime limits and feature
bits. `out_capabilities` must be non-null.

Required fields:

- `page_size`: page size used by runtime page allocation. Must be non-zero.
- `block_size`: logical block size for block I/O. May be zero if no block device
  is attached.
- `max_io_request_size`: largest accepted single request length. Must be
  non-zero when block I/O is supported.
- `max_io_batch_size`: largest accepted request count for one batch. Must be
  non-zero.
- `max_outstanding_io`: maximum submitted requests that may be pending
  completion. Must be at least `max_io_batch_size`.

Feature bits describe available behavior. A caller must not assume unsupported
features work merely because a callback exists.

## Page Allocation

`allocate_pages(context, page_count, alignment, flags, out_allocation)` allocates
a runtime-owned page span.

Input rules:

- `page_count` must be greater than zero.
- `alignment == 0` means the runtime default page alignment.
- Non-zero `alignment` must be a power of two.
- `out_allocation` must be non-null.
- Unknown `flags` bits must be rejected with
  `MATTSQL_ABI_STATUS_INVALID_ARGUMENT`.
- Unsupported known flags must be rejected with
  `MATTSQL_ABI_STATUS_NOT_SUPPORTED`.

Allocation result rules:

- `data` must be non-null on success.
- `page_count`, `page_size`, `alignment`, and `flags` must describe the returned
  allocation.
- `page_size` must match the runtime capability page size.
- If `MATTSQL_ABI_MEMORY_ZEROED` is set, every byte in the allocation must be
  zeroed before success is returned.
- If `MATTSQL_ABI_MEMORY_DMA` is set and accepted, `physical_address` must be
  meaningful for the backend's device DMA model.
- If physical addresses are unsupported, `physical_address` must be zero.

Ownership:

- The runtime provider owns the allocation.
- The caller may read and write the returned memory until it calls `free_pages`.
- The caller must free exactly the allocation metadata returned by
  `allocate_pages`; it must not fabricate or partially modify allocation
  metadata before freeing.
- A page allocation must not be freed while any submitted I/O request still
  references it.

`free_pages(context, allocation)` releases an allocation. It must reject null
allocation metadata or null `allocation->data`.

## I/O Submission

`submit_io_batch(context, requests, request_count, out_submission)` submits one
or more block I/O requests.

Input rules:

- `requests` must be non-null.
- `request_count` must be greater than zero.
- `request_count` must be less than or equal to `max_io_batch_size`.
- `out_submission` must be non-null.
- Each request offset must be aligned to `block_size`.
- For `READ` and `WRITE`, `buffer` must be non-null and `buffer_length` must be
  large enough for the effective request length.
- For `FLUSH`, `buffer` may be null and `buffer_length` may be zero.
- Unknown operations or request flags must be rejected with
  `MATTSQL_ABI_STATUS_INVALID_ARGUMENT`.
- Unsupported known flags must be rejected with
  `MATTSQL_ABI_STATUS_NOT_SUPPORTED`.

Effective request length:

- If `request.length != 0`, use `request.length`.
- If `request.length == 0`, use `request.buffer_length`.
- For `READ` and `WRITE`, the effective length must be greater than zero and
  block-aligned.
- For `FLUSH`, the effective length may be zero only if the backend documents
  that zero means flush all pending writes. Otherwise zero-length flushes must be
  rejected.

Request IDs:

- If `request.id != 0`, the backend must echo that ID in the completion.
- If `request.id == 0`, the backend assigns a non-zero request ID.
- `out_submission.first_request_id` is the ID used for `requests[0]`.
- Assigned request IDs must not be reused while an earlier request with that ID
  can still complete.

Submission policy:

- Version 1 uses all-or-error submission.
- On success, every request in the batch has been accepted and
  `out_submission.submitted_count == request_count`.
- On failure, no request in the batch may be accepted and no completion may be
  produced for that failed call.
- Backpressure should be reported as `MATTSQL_ABI_STATUS_IO_ERROR` unless a more
  specific status code is later added.

Buffer lifetime:

- For `WRITE`, the request buffer must remain valid and immutable until the
  matching completion is delivered.
- For `READ`, the request buffer must remain valid and writable until the
  matching completion is delivered.
- For `FLUSH`, no data buffer lifetime is required.
- The backend must not retain pointers after completing the request.

## I/O Completion

`poll_io_completions(context, completions, max_completion_count,
out_completion_count)` returns completed requests.

Input rules:

- `completions` must be non-null.
- `max_completion_count` must be greater than zero.
- `out_completion_count` must be non-null.

Output rules:

- `*out_completion_count` may be zero if no completions are ready.
- `*out_completion_count` must not exceed `max_completion_count`.
- Each completion must include the request ID and `user_data` from the matching
  request.
- `bytes_transferred` must be the number of bytes read or written on successful
  `READ` or `WRITE`.
- `bytes_transferred` must be zero for `FLUSH`.
- A failed request must complete exactly once with an error status.

Completion ordering:

- A backend should preserve completion order for requests that require ordering
  through barriers or flushes.
- A backend may complete independent reads or writes out of submission order
  unless a barrier flag or device ordering rule prevents it.

## Flush and Barrier Semantics

`MATTSQL_ABI_IO_FLUSH` makes prior writes durable according to the backend's
storage model.

`MATTSQL_ABI_IO_REQUEST_BARRIER_BEFORE` means the request must not begin until
all previously submitted writes that are visible to the same device queue have
completed in the required storage order.

`MATTSQL_ABI_IO_REQUEST_BARRIER_AFTER` means later requests must not begin until
this request has completed in the required storage order.

`MATTSQL_ABI_IO_REQUEST_FORCE_UNIT_ACCESS` requests durable write completion for
that write without relying on a later flush. A backend that cannot provide that
semantic must reject the request with `MATTSQL_ABI_STATUS_NOT_SUPPORTED`.

Minimum WAL-safe sequence:

1. Submit WAL write.
2. Submit WAL flush or a write with force-unit-access if supported.
3. Wait for the WAL durability completion.
4. Submit dependent data-page writes.

A backend must not report the WAL durability completion before the WAL bytes are
durable under its storage model.

## Time

`monotonic_nanos(context, out_nanos)` returns monotonic runtime time in
nanoseconds. `out_nanos` must be non-null.

The value must not move backward during one boot or process lifetime. It is not
wall-clock time and has no required epoch.

## Yield

`yield(context)` gives the runtime an opportunity to run other work. In version
1, yield has no fairness or scheduling guarantee. It may be a no-op.

## Logging

`log(context, level, message, message_length)` receives borrowed diagnostic text.
The message is not required to be NUL-terminated and is valid only for the
duration of the call.

Logging must not throw, panic, or reenter database execution.

## Panic

`panic(context, message, message_length)` reports a fatal runtime failure. It may
abort, halt, or otherwise never return.

If a backend returns from `panic`, the C++ adapter will abort the process.

## Threading and Reentrancy

Version 1 is single-runtime-table oriented. Unless a backend documents stronger
guarantees, callbacks are not assumed to be thread-safe.

The C++ database must not call ABI callbacks concurrently through the same
runtime table until the backend advertises and documents that support.

Callbacks must not call back into the C++ database engine unless a future ABI
explicitly defines such reentrancy.

## Exclusions

The following are intentionally outside ABI version 1:

- task creation
- task groups
- timer registration
- file paths or filesystems
- networking
- SQL parsing or execution APIs
- catalog or storage-format APIs
- interrupt or device-driver registration
