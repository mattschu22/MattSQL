# MattSQL Code-Level Invariants

This document defines well-formedness rules for the objects that cross module
boundaries. A module may assume these invariants hold for objects it receives
from the previous layer. Constructors, factories, planners, optimizers, codecs,
and mutating methods must either preserve these invariants or return a failing
`Status` / `Result<T>`.

Related docs:

- [Docs index](../README.md)
- [Overview](../overview.md)
- [Current state](../status/current_state.md)
- [Runtime C ABI](runtime_abi.md)

The invariants are intended to be cheap to maintain. They should be enforced at
construction, binding, planning, decoding, page-validation, or ABI-boundary
points rather than through repeated hot-path defensive checks.

## General Object Rules

- Public module APIs return errors through `Status` or `Result<T>`.
- A successful `Result<T>` has `status.code == ErrorCode::Ok` and contains a
  value.
- A failed `Result<T>` has `status.code != ErrorCode::Ok`; callers must ignore
  any value field.
- A successful `Status` has `code == ErrorCode::Ok` and an empty or ignored
  message.
- Public APIs do not return null owning pointers on success.
- Borrowed spans, views, and string views never outlive the object or callback
  that owns their memory.
- IDs use their invalid sentinel where one exists. `kInvalidPageId` and
  `kInvalidLsn` must never be treated as valid durable identifiers.
- Object `Kind()` accessors reflect the concrete runtime type. Callers may use
  `Kind()` as a dispatch guard, but the dynamic type must still agree.
- No module stores references to mutable objects owned by an earlier pipeline
  stage unless the API explicitly says the reference is borrowed.

## Common Values And Identifiers

### `SqlType`

- `SqlType::Null` is a value type for SQL `NULL`, not a valid physical storage
  column type.
- Durable table columns use `Integer`, `Text`, or `Boolean`.
- Expression result types are assigned by binding and must not be left as
  `Null` except for literal `NULL` or expressions whose current semantics
  genuinely produce only `NULL`.

### `Value`

- The variant alternative determines the value's runtime type.
- `NullValue` is the only representation of SQL `NULL`.
- `std::int64_t` values are the only integer representation.
- `bool` values are the only boolean representation after binding has completed.
- `std::string` values are owned text values; storage code should not retain
  references into them.

### Identifiers

- Name lookup keys are produced with `FoldIdentifierKey`.
- User-visible names preserve source/catalog spelling.
- Case-insensitive equality for table, column, and index names is based on the
  folded key, not direct string comparison.

## Lexer Objects

### `SourceLocation`

- `line` and `column` are 1-based.
- `offset` is 0-based.
- Locations advance monotonically through the input.

### `SourceRange`

- `start` points to the first source character in the token.
- `end` points one character past the token.
- Trivia is not included in token ranges.

### `Token`

- The token stream is in source order and ends with exactly one
  `TokenType::EndOfFile` token.
- `Token::lexeme` preserves the exact source spelling except for the EOF token,
  whose lexeme is empty.
- `Invalid` tokens preserve the source lexeme that made them invalid.
- Keyword recognition is case-insensitive; keyword lexemes still preserve
  source casing.

## Parser And AST Objects

### Parser Output

- A parsed `StatementPtr` owns the full AST for exactly one SQL statement.
- Parsing either returns a complete statement or throws/returns a parse error;
  it does not return partial ASTs.
- Parser output contains syntax only. It must not contain catalog IDs, resolved
  types, storage references, runtime handles, or execution state.

### AST Nodes

- AST expression and statement trees have a single owner through `unique_ptr`.
- Literal AST nodes store decoded values, not raw literal token text.
- `ColumnRef::name` is a source-level name and may contain dotted components;
  it is not resolved. The binder decides which dotted forms are valid.
- `SelectStatement::table_name` is empty only when `FROM` is omitted.
- `SelectStatement::where` is null only when `WHERE` is omitted.
- `SelectProjection::alias` is empty only when no alias was provided.
- `InsertStatement::values` contains exactly the parsed `VALUES` expression
  list.
- `CreateTableStatement::columns` contains parsed names and type names only;
  column IDs are assigned later.

## Catalog Objects

### `ColumnSchema`

- `name` is non-empty.
- `type` is not `SqlType::Null` for stored table columns.
- `id` is the zero-based ordinal within `TableSchema::columns`.
- IDs are contiguous: `schema.columns[i].id == i`.
- `nullable` describes storage and insert validation semantics.

### `TableSchema`

- `columns` is non-empty for stored user tables.
- Folded column names are unique within a table.
- Column order is stable and defines tuple encoding order.

### `TableInfo`

- `id` is a stable catalog-assigned table identifier.
- `name` is non-empty.
- `schema` satisfies `TableSchema` invariants.
- `heap_root_page_id == kInvalidPageId` means physical heap storage has not
  been assigned yet.
- Any valid `heap_root_page_id` names the root page for the table's heap
  storage.
- `indexes` contains only indexes whose `table_id == id`.

### `IndexSchema` And `IndexInfo`

- Index names are non-empty.
- `key_columns` is non-empty.
- Every key column ID is in range for the owning table schema.
- Key column IDs are unique within one index schema.
- `IndexInfo::table_id` names the owning table.
- `root_page_id == kInvalidPageId` means physical index storage has not been
  assigned yet.

### `CreateTableRequest`

- `name` is non-empty.
- `schema` satisfies table schema invariants except that catalog creation may
  normalize or reassign column IDs.

## Bound Objects

### `BoundExpression`

- `Kind()` agrees with the concrete expression type.
- `type` is assigned by the binder and is meaningful to downstream modules.
- Child expression pointers in unary and binary expressions are non-null.
- `BoundStarExpression` does not cross into logical planning except as an
  explicit error case; normal binding expands stars into columns.

### `BoundLiteralExpression`

- `value` matches `type`.
- `NullValue` uses `SqlType::Null`.

### `BoundColumnExpression`

- `table_id` names the resolved table.
- `column_id` is in range for the resolved table schema.
- `table_name` and `column_name` are non-empty user-visible names.
- `type` matches the resolved column schema type.

### `BoundSelectStatement`

- `Kind()` returns `Select`.
- `projections` is non-empty after binding.
- Every projection pointer is non-null and fully typed.
- `projection_names.size() == projections.size()`.
- Empty projection names mean the executor should derive a display name.
- `table.name` is empty only for scalar selects without `FROM`.
- `where` is null only when no predicate exists; otherwise it is boolean.

### `BoundInsertStatement`

- `Kind()` returns `Insert`.
- `table` is a valid resolved table.
- `values.size() == table.schema.columns.size()`.
- Every value expression is non-null, fully typed, and contains no column
  references.
- Each value is coercible to the corresponding column type. Any binder-permitted
  coercion is represented directly in the bound expression.

### `BoundCreateTableStatement`

- `Kind()` returns `CreateTable`.
- `request` satisfies `CreateTableRequest` invariants.
- Folded table and column-name conflicts have been checked before the statement
  reaches planning.

## Logical Plan Objects

### `LogicalPlan`

- `Kind()` agrees with the concrete node type.
- Every child pointer is non-null.
- Logical nodes contain semantic intent but no concrete storage handles.
- Logical expression subtrees are owned by the node that references them.

### Logical Node Arity

- `LogicalCreateTable`, `LogicalValues`, and `LogicalSeqScan` are leaves.
- `LogicalFilter` has exactly one child.
- `LogicalProjection` has exactly one child.
- `LogicalInsert` has exactly one child.

### `LogicalSeqScan`

- `table` is a valid resolved table.
- `table.name` is non-empty.

### `LogicalFilter`

- `predicate` is non-null.
- `predicate->type == SqlType::Boolean`.

### `LogicalProjection`

- `projections` is non-empty.
- Every projection pointer is non-null and fully typed.
- `projection_names` is either empty or has the same size as `projections`.

### `LogicalValues`

- `rows` contains bound scalar expressions only.
- Expressions in `rows` are non-null and contain no column references.
- If `tuples` is populated, it represents the same logical rows as `rows`.
- Scalar `SELECT` uses exactly one empty input row.
- An empty `LogicalValues` node represents an empty input relation.

### `LogicalInsert`

- `table` is a valid resolved table.
- The child produces rows compatible with `table.schema`.

### `LogicalCreateTable`

- `request` satisfies `CreateTableRequest` invariants.

## Optimizer Rules

### `OptimizerRule`

- `Name()` is non-empty and stable.
- Rule names are unique within an optimizer instance.
- `Apply()` returns a non-null logical plan on success.
- Rules do not mutate catalog, storage, runtime, or transaction state.
- Rewrites preserve bound expression types and logical operator arity.
- Rewrites that would convert an execution-time error into a planning-time error
  must not be applied unless that semantic change is intentional and tested.

## Physical Plan Objects

### `PhysicalPlan`

- `Kind()` agrees with the concrete node type.
- Every child pointer is non-null.
- Physical nodes contain executable access metadata but do not own catalog,
  storage manager, runtime, or transaction objects.

### Physical Node Arity

- `PhysicalCreateTable`, `PhysicalValues`, and `PhysicalSeqScan` are leaves.
- `PhysicalFilter` has exactly one child.
- `PhysicalProjection` has exactly one child.
- `PhysicalInsert` has exactly one child.

### `TableStorageReference`

- `method` identifies the storage implementation to open.
- `table_id` names the catalog table.
- `table_name` is non-empty.
- `root_page_id` is valid before table data access.
- `schema` is the schema used to encode and decode records for this table.

### `PhysicalSeqScan`

- `table` is a valid resolved table.
- `storage` names the same table and schema as `table`.
- `storage.root_page_id` is valid before execution opens the heap.

### `PhysicalInsert`

- `table` is a valid resolved table.
- `storage` names the same table and schema as `table`.
- The child produces rows with width equal to `table.schema.columns.size()`.

### `PhysicalFilter`

- `predicate` is non-null and boolean.

### `PhysicalProjection`

- `projections` is non-empty.
- Every projection pointer is non-null and fully typed.
- `projection_names` is either empty or has the same size as `projections`.

### `PhysicalValues`

- `rows` contains non-null scalar expressions with no column references.
- If `tuples` is populated, it represents the same logical rows as `rows`.

### `PhysicalCreateTable`

- `request` satisfies `CreateTableRequest` invariants.
- `storage_method` names a supported storage method before execution.

## Execution Objects

### `EvaluationContext`

- `schema` and `row` are either both null for scalar expression evaluation or
  both non-null for row evaluation.
- When non-null, `row->size() == schema->columns.size()`.
- Column IDs in bound column expressions index both the schema and row.

### `QueryResult`

- `columns.empty()` means the statement produced command status rather than a
  visible row set.
- For row-producing statements, every row has `row.size() == columns.size()`.
- Values are materialized for presentation and tests; storage code should not
  consume `QueryResult` as an internal data path.

### `VectorBatch` And `ColumnVector`

- Every `ColumnVector` in a batch has `value_count == VectorBatch::row_count`.
- `NullBitmap` contains at least enough bits for `value_count` values when nulls
  are represented.
- Fixed-width column data is sized according to type and `value_count`.
- Variable-width columns use `variable_offsets` with `value_count + 1` entries
  when populated.
- `SqlType::Null` columns are allowed only for explicitly all-null vectors, not
  for stored table schema columns.

## Tuple Encoding Objects

### `Tuple`

- `bytes` is an owned encoded record.
- A tuple's interpretation is valid only with the schema used to encode it.

### `TupleBatch`

- `offsets` is empty only when the batch has no populated tuple payload.
- When populated, row count is derived as `offsets.size() - 1`.
- Offsets are monotonically nondecreasing and every offset is within `data`.

### `TupleCodec`

- `Encode(schema, values)` succeeds only when value count equals column count.
- Encoded fields appear in schema column order.
- Encoded field tags match the schema type or `NULL` for nullable columns.
- Non-nullable columns never encode `NULL`.
- `Decode(schema, record)` consumes the entire record; trailing bytes are
  corruption.
- Decode errors from malformed bytes use `ErrorCode::Corruption`.

## Page And Record Objects

### `PageHeader`

- `page_id` is valid for allocated pages.
- `kind != PageKind::Free` for initialized structured pages.
- `lower <= upper`.
- `upper` is no greater than the payload byte size.
- `page_lsn` is either `kInvalidLsn` for never-logged pages or the newest LSN
  whose effects are represented on the page.

### `PageView` And `ConstPageView`

- `header` is non-null.
- `bytes.bytes` is non-empty.
- The header and bytes refer to the same physical/logical page.
- Mutable page views require write ownership or a write pin from the caller.

### `SlottedPage`

- `Initialize()` sets `kind`, resets flags and LSN, sets `lower == 0`, and sets
  `upper == bytes.size()`.
- Slot directory entries are stored from the beginning of the page payload.
- Record bytes are stored from the end of the page payload backward.
- `lower` is always aligned to the slot-entry size.
- Active slots point inside the record area and outside the slot directory.
- Deleted slots remain addressable but read as `NotFound`.
- Inserting into a deleted slot may reuse the slot ID.
- `RecordId` remains stable for an active record until that record is deleted.

### `RecordId`

- `page_id` is valid.
- `slot_id` names a slot in the page's slot directory.
- A `RecordId` identifies encoded record bytes, not decoded SQL values.

### `RecordView`

- `id` identifies the record being viewed.
- `bytes` points into heap/page-owned memory.
- The view remains valid only while the owning page/table/cursor contract keeps
  that memory stable.

## Heap And Table Storage Objects

### `HeapTable`

- `Insert()` receives one encoded record and returns a stable `RecordId`.
- `Read()` returns exactly the encoded bytes for the requested active record.
- `Delete()` makes the record invisible to later reads and scans.
- `Scan()` returns active records in implementation-defined but stable cursor
  order for that scan.
- Heap tables do not evaluate predicates or know SQL projection semantics.

### `HeapCursor`

- `Next()` returns one active record per success.
- End of scan is represented by `ErrorCode::NotFound`.
- Other errors are real failures and must not be treated as end of scan.

### `TableStorageManager`

- `CreateHeap()` creates storage for an existing catalog table and returns a
  valid root page ID.
- `OpenHeap()` succeeds only when the reference names an existing heap with a
  matching table ID and root page ID.
- Storage managers do not create catalog metadata.

## Block Device And Buffer Pool Objects

### `BlockDevice`

- `BlockSize()` is non-zero for usable devices.
- `SizeBytes()` is a multiple of `BlockSize()`.
- Reads, writes, and flushes are block-aligned.
- Access ranges are within device bounds.
- A successful write makes the bytes visible to later reads from the same
  device.

### `PageHandle`

- A handle owns one page pin.
- `Id()` names the pinned page.
- `Page()` is valid while the handle is alive.
- `MutablePage()` is used only for write pins.
- `MarkDirty(lsn)` records the newest LSN needed before the page can be flushed.

### `BufferPool`

- `NewPage()` returns a pinned page initialized for the requested kind.
- `PinPage()` returns a pinned handle for an existing page.
- Dirty pages are not flushed before their page LSN is WAL-durable.
- Eviction must not occur while a page has live pins.

## WAL Objects

### `WalRecord`

- `type` identifies the payload interpretation.
- `transaction_id` is valid for transaction-scoped records.
- Page-oriented records have a valid `page_id`.
- Record-oriented records have a valid `RecordId`.
- `payload` is borrowed and valid only for the duration of `Append()`.

### `WalManager`

- `Append()` assigns monotonically increasing LSNs.
- `Flush(lsn)` makes all records through `lsn` durable before returning success.
- `DurableLsn()` never moves backward.
- `Recover()` operates only on durable WAL records.
- WAL flush/barrier behavior is strong enough to enforce WAL-before-data.

## Transaction Objects

### `Transaction`

- `Id()` is stable for the transaction lifetime.
- `Mode()` is stable for the transaction lifetime.
- `State()` transitions forward only: active to committed, rolled back, or
  aborted.
- `BeginLsn()` is valid once the transaction has begun durable work; otherwise
  it may be `kInvalidLsn` or the implementation's documented initial LSN.

### `TransactionManager`

- `Begin()` returns an active transaction on success.
- `Commit()` is valid only for active transactions.
- `Rollback()` is valid only for active transactions.
- Read-only transactions do not perform writes.
- The initial concurrency model may enforce a single active writer.

## Runtime Objects

### `RuntimeCapabilities`

- `page_size` is non-zero.
- If block I/O is supported, `block_size` is non-zero.
- `max_io_batch_size` is non-zero.
- `max_outstanding_io >= max_io_batch_size`.
- Feature bits are authoritative; callers must not assume unsupported behavior.

### `RuntimePageAllocation`

- On success, `data` is non-null.
- `page_count` and `page_size` are non-zero.
- `alignment` is non-zero and a power of two.
- `page_size` matches runtime capabilities.
- If `kRuntimeMemoryZeroed` was accepted, all bytes are zeroed before success.
- If physical addresses are unsupported, `physical_address == 0`.
- The exact allocation metadata returned by the runtime is the metadata passed
  to `FreePages()`.

### `RuntimePageAllocationHandle`

- The handle is move-only.
- A non-empty handle owns exactly one runtime page allocation.
- Destruction or `Reset()` frees the allocation at most once.
- `Release()` transfers responsibility to the caller and empties the handle.
- `bytes()` spans `page_count * page_size` bytes.

### `IoRequest`

- `operation` is `Read`, `Write`, or `Flush`.
- Unknown flag bits are rejected.
- For read/write, `buffer.bytes` is non-empty and large enough for the effective
  length.
- Effective length is `length` when non-zero, otherwise `buffer.bytes.size()`.
- Read/write effective length is non-zero and block-aligned.
- Offsets are block-aligned.
- `id == 0` lets the runtime assign an ID; non-zero IDs must be echoed.
- Read buffers remain valid and writable until completion.
- Write buffers remain valid and immutable until completion.

### `IoSubmissionResult`

- On success, `submitted_count` equals the requested batch size.
- `first_request_id` is non-zero and is the ID used for the first request.
- Batch submission is all-or-error.

### `IoCompletion`

- Every accepted request completes exactly once.
- `id` and `user_data` match the submitted request.
- Successful read/write completions report bytes transferred.
- Flush completions report zero bytes transferred.
- Failed completions preserve the request identity and carry a failing status.

### `PlatformRuntime`

- `SubmitIo()` and `PollIoCompletion()` are helper wrappers over the batch
  boundary.
- `MonotonicNanos()` never moves backward during one runtime lifetime.
- `Log()` must not reenter SQL execution.
- `Panic()` does not return. If a backend panic callback returns, the adapter
  aborts.
- Runtime callbacks are not assumed thread-safe unless the backend documents
  stronger guarantees.

## C ABI Objects

### ABI General Rules

- All reserved fields are initialized to zero by callers and ignored by callees.
- No C++ classes, templates, exceptions, STL containers, virtual calls, Rust
  references, Rust panics, or Rust traits cross the ABI.
- Strings and diagnostics are pointer-plus-length buffers and are not assumed
  to be NUL-terminated.
- Status messages are borrowed and valid only for the documented callback
  lifetime.
- C++ exceptions and Rust panics must not unwind across the ABI boundary.

### `mattsql_abi_runtime_v1`

- `version == MATTSQL_ABI_RUNTIME_VERSION`.
- Required callbacks are non-null:
  `get_capabilities`, `allocate_pages`, `free_pages`, `submit_io_batch`,
  `poll_io_completions`, `monotonic_nanos`, `log`, and `panic`.
- `yield` is optional; a missing callback means no-op yield.
- ABI version 1 field order, field sizes, alignments, constants, and callback
  meanings are frozen.
- Breaking changes require a new ABI version and matching adapter, layout test,
  Rust mirror, and documentation updates.

## SQL Engine Facade

### `SqlEngine`

- `Execute()` accepts exactly one SQL statement. Script splitting belongs to the
  CLI or a future script layer.
- The facade runs the pipeline in order: lex, parse, bind, logical plan,
  optimize, physical plan, execute.
- Parse exceptions are converted to `ErrorCode::ParseError` before leaving the
  facade.
- Injected catalog, storage, and runtime components must outlive the engine.
- The default engine owns its default hosted components.
