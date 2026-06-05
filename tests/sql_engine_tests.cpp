#include "mattsql/common/result_utils.hpp"
#include "mattsql/sql_engine.hpp"

#include "test_framework.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <variant>

namespace {

class ReadOnlyTransaction final : public mattsql::Transaction {
public:
  mattsql::TransactionId Id() const override { return 99; }
  mattsql::TransactionMode Mode() const override {
    return mattsql::TransactionMode::ReadOnly;
  }
  mattsql::TransactionState State() const override {
    return mattsql::TransactionState::Active;
  }
  mattsql::LogSequenceNumber BeginLsn() const override {
    return mattsql::LogSequenceNumber{0};
  }
};

} // namespace

/// Verifies the default SQL engine can select a numeric literal.
TEST_CASE(default_sql_engine_selects_numeric_literal) {
  mattsql::DefaultSqlEngine engine;

  const auto result = engine.Execute("SELECT 1;");

  EXPECT_TRUE(mattsql::status_ok(result.status));
  EXPECT_EQ(result.value->columns.size(), 1U);
  EXPECT_EQ(result.value->rows.size(), 1U);
  EXPECT_EQ(result.value->columns[0], std::string("expr1"));
  EXPECT_EQ(std::get<std::int64_t>(result.value->rows[0][0]), std::int64_t{1});
}

/// Verifies multiple literal projections and aliases flow through planning.
TEST_CASE(default_sql_engine_selects_multiple_literals_with_aliases) {
  mattsql::DefaultSqlEngine engine;

  const auto result = engine.Execute("select 42 as answer, 'MattSQL' as name, false");

  EXPECT_TRUE(mattsql::status_ok(result.status));
  EXPECT_EQ(result.value->columns.size(), 3U);
  EXPECT_EQ(result.value->rows.size(), 1U);
  EXPECT_EQ(result.value->columns[0], std::string("answer"));
  EXPECT_EQ(result.value->columns[1], std::string("name"));
  EXPECT_EQ(result.value->columns[2], std::string("expr3"));
  EXPECT_EQ(std::get<std::int64_t>(result.value->rows[0][0]), std::int64_t{42});
  EXPECT_EQ(std::get<std::string>(result.value->rows[0][1]), std::string("MattSQL"));
  EXPECT_EQ(std::get<bool>(result.value->rows[0][2]), false);
}

/// Verifies SQL string escaping follows parser semantics.
TEST_CASE(default_sql_engine_decodes_escaped_string_literals) {
  mattsql::DefaultSqlEngine engine;

  const auto result = engine.Execute("SELECT 'Matt''SQL' AS name;");

  EXPECT_TRUE(mattsql::status_ok(result.status));
  EXPECT_EQ(result.value->columns.size(), 1U);
  EXPECT_EQ(result.value->rows.size(), 1U);
  EXPECT_EQ(result.value->columns[0], std::string("name"));
  EXPECT_EQ(std::get<std::string>(result.value->rows[0][0]), std::string("Matt'SQL"));
}

/// Verifies aliases are parsed as SQL, not by raw substring searches.
TEST_CASE(default_sql_engine_handles_as_inside_string_literals) {
  mattsql::DefaultSqlEngine engine;

  const auto result = engine.Execute("SELECT 'x as y' AS value;");

  EXPECT_TRUE(mattsql::status_ok(result.status));
  EXPECT_EQ(result.value->columns.size(), 1U);
  EXPECT_EQ(result.value->rows.size(), 1U);
  EXPECT_EQ(result.value->columns[0], std::string("value"));
  EXPECT_EQ(std::get<std::string>(result.value->rows[0][0]), std::string("x as y"));
}

/// Verifies the hosted default engine supports CREATE, INSERT, and SELECT.
TEST_CASE(default_sql_engine_executes_in_memory_table_queries) {
  mattsql::DefaultSqlEngine engine;

  EXPECT_TRUE(mattsql::status_ok(
      engine.Execute("CREATE TABLE users (id INT, name TEXT, active BOOL);").status));
  EXPECT_TRUE(mattsql::status_ok(
      engine.Execute("INSERT INTO users VALUES (1, 'Ada', true);").status));
  EXPECT_TRUE(mattsql::status_ok(
      engine.Execute("INSERT INTO users VALUES (2, 'Grace', false);").status));

  const auto result = engine.Execute("SELECT id, name FROM users WHERE active = true;");

  EXPECT_TRUE(mattsql::status_ok(result.status));
  EXPECT_EQ(result.value->columns.size(), 2U);
  EXPECT_EQ(result.value->rows.size(), 1U);
  EXPECT_EQ(result.value->columns[0], std::string("id"));
  EXPECT_EQ(result.value->columns[1], std::string("name"));
  EXPECT_EQ(std::get<std::int64_t>(result.value->rows[0][0]), std::int64_t{1});
  EXPECT_EQ(std::get<std::string>(result.value->rows[0][1]), std::string("Ada"));
}

/// Verifies the engine surfaces errors as Status values.
TEST_CASE(default_sql_engine_returns_status_errors) {
  mattsql::DefaultSqlEngine engine;

  EXPECT_TRUE(engine.Execute("SELECT 1 AS one FROM missing;").status.code ==
              mattsql::ErrorCode::NotFound);
  EXPECT_TRUE(engine.Execute("DROP TABLE users;").status.code ==
              mattsql::ErrorCode::ParseError);
  EXPECT_TRUE(engine.Execute("SELECT 'missing end;").status.code ==
              mattsql::ErrorCode::ParseError);
}

/// Verifies the most negative signed integer literal parses as a valid value.
TEST_CASE(default_sql_engine_accepts_int64_min_literal) {
  mattsql::DefaultSqlEngine engine;

  const auto result = engine.Execute("SELECT -9223372036854775808 AS min_value;");

  EXPECT_TRUE(mattsql::status_ok(result.status));
  EXPECT_EQ(result.value->rows.size(), 1U);
  EXPECT_EQ(std::get<std::int64_t>(result.value->rows[0][0]),
            std::numeric_limits<std::int64_t>::min());
}

/// Verifies integer addition overflow fails instead of wrapping.
TEST_CASE(default_sql_engine_rejects_integer_addition_overflow) {
  mattsql::DefaultSqlEngine engine;

  const auto result = engine.Execute("SELECT 9223372036854775807 + 1;");

  EXPECT_TRUE(result.status.code == mattsql::ErrorCode::ExecutionError);
}

/// Verifies integer subtraction underflow fails instead of wrapping.
TEST_CASE(default_sql_engine_rejects_integer_subtraction_underflow) {
  mattsql::DefaultSqlEngine engine;

  const auto result = engine.Execute("SELECT -9223372036854775807 - 2;");

  EXPECT_TRUE(result.status.code == mattsql::ErrorCode::ExecutionError);
}

/// Verifies integer multiplication overflow fails instead of wrapping.
TEST_CASE(default_sql_engine_rejects_integer_multiplication_overflow) {
  mattsql::DefaultSqlEngine engine;

  const auto result = engine.Execute("SELECT 9223372036854775807 * 2;");

  EXPECT_TRUE(result.status.code == mattsql::ErrorCode::ExecutionError);
}

/// Verifies unary negation overflow fails instead of returning INT64_MIN.
TEST_CASE(default_sql_engine_rejects_unary_negation_overflow) {
  mattsql::DefaultSqlEngine engine;

  const auto result = engine.Execute("SELECT -(-9223372036854775807 - 1);");

  EXPECT_TRUE(result.status.code == mattsql::ErrorCode::ExecutionError);
}

/// Verifies INT64_MIN / -1 fails instead of overflowing.
TEST_CASE(default_sql_engine_rejects_integer_division_overflow) {
  mattsql::DefaultSqlEngine engine;

  const auto result = engine.Execute("SELECT (-9223372036854775807 - 1) / -1;");

  EXPECT_TRUE(result.status.code == mattsql::ErrorCode::ExecutionError);
}

/// Verifies SQL NOT binds looser than comparison operators.
TEST_CASE(default_sql_engine_parses_not_at_comparison_precedence) {
  mattsql::DefaultSqlEngine engine;

  const auto result = engine.Execute("SELECT NOT 1 = 2 AS should_be_true;");

  EXPECT_TRUE(mattsql::status_ok(result.status));
  EXPECT_EQ(result.value->rows.size(), 1U);
  EXPECT_EQ(std::get<bool>(result.value->rows[0][0]), true);
}

/// Verifies read-only transactions can still execute read statements.
TEST_CASE(default_sql_engine_allows_reads_in_read_only_transactions) {
  mattsql::DefaultSqlEngine engine;

  EXPECT_TRUE(
      mattsql::status_ok(engine.Execute("CREATE TABLE users (id INT);").status));
  EXPECT_TRUE(
      mattsql::status_ok(engine.Execute("INSERT INTO users VALUES (7);").status));

  ReadOnlyTransaction transaction;
  const auto result = engine.Execute("SELECT id FROM users;", transaction);

  EXPECT_TRUE(mattsql::status_ok(result.status));
  EXPECT_EQ(result.value->rows.size(), 1U);
  EXPECT_EQ(std::get<std::int64_t>(result.value->rows[0][0]), std::int64_t{7});
}

/// Verifies caller-supplied transactions are honored.
TEST_CASE(default_sql_engine_uses_supplied_transaction) {
  mattsql::DefaultSqlEngine engine;
  ReadOnlyTransaction transaction;

  const auto result = engine.Execute("CREATE TABLE users (id INT);", transaction);

  EXPECT_TRUE(result.status.code == mattsql::ErrorCode::TransactionConflict);
}
