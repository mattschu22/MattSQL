# Cleanup Report

This pass focused on reducing repeated code while preserving the current
architecture and behavior.

## Changes

- Added shared identifier normalization in `include/mattsql/common/identifier.hpp`.
  Lexer, parser, binder, and catalog code now use one case-folding helper.

- Added shared scalar/status helpers in `include/mattsql/common/status.hpp` and
  `include/mattsql/common/value_utils.hpp`. CLI output, SQL logic tests,
  executor evaluation, optimizer folding, and tuple encoding now share common
  value formatting, type inspection, comparison, and error-code naming.

- Added bound-expression utilities in `include/mattsql/binder/expression_utils.hpp`
  and `src/binder/expression_utils.cpp`. Logical and physical planning now share
  expression cloning and no-column-reference validation; optimizer constant
  folding uses the shared transform traversal.

- Split SQL logic support out of `tests/sql_logic_tests.cpp` into
  `tests/sql_logic_support.cpp` and `tests/sql_logic_support.hpp`. The test file
  now only registers the integration-script and deterministic-fuzzer cases.

- Added `tests/mattsql_test_utils.hpp` for repeated byte-span and byte-equality
  helpers used by block-device and runtime tests.

- Documented the project state in `docs/current_state.md`.

## Tradeoffs

- The expression utility module adds one more source file, but removes duplicate
  clone/traversal code from both planners and gives optimizer rules a single
  traversal primitive to build on.

- `value_utils.hpp` centralizes user-visible scalar formatting. That is useful
  for CLI and tests, but it also makes the current display format a shared
  convention. Future SQL output changes should update this helper deliberately.

- Identifier folding remains simple ASCII-compatible lowercase behavior. This
  matches the current lexer and catalog assumptions; full SQL collation or
  Unicode identifier behavior is still out of scope.

- SQL logic support is now split into more files. The benefit is easier review:
  test registration, script loading, output comparison, and deterministic fuzzing
  are no longer all in one oversized test source.

- The C ABI adapter remains explicit. Conversion-heavy ABI boundary code is
  intentionally not abstracted aggressively because preserving field-by-field
  validation is clearer and safer than hiding it behind generic helpers.
