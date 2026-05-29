#include "mattsql/engine.hpp"

#include "test_framework.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <variant>

/// Verifies the prototype engine can select a numeric literal.
TEST_CASE(selects_numeric_literal) {
  mattsql::Engine engine;

  const auto result = engine.execute("SELECT 1;");

  EXPECT_EQ(result.columns.size(), 1U);
  EXPECT_EQ(result.rows.size(), 1U);
  EXPECT_EQ(result.columns[0], std::string("expr1"));
  EXPECT_EQ(std::get<std::int64_t>(result.rows[0][0]), std::int64_t{1});
}

/// Verifies the prototype engine handles multiple literal projections.
TEST_CASE(selects_multiple_literals_with_aliases) {
  mattsql::Engine engine;

  const auto result = engine.execute("select 42 as answer, 'MattSQL' as name");

  EXPECT_EQ(result.columns.size(), 2U);
  EXPECT_EQ(result.rows.size(), 1U);
  EXPECT_EQ(result.columns[0], std::string("answer"));
  EXPECT_EQ(result.columns[1], std::string("name"));
  EXPECT_EQ(std::get<std::int64_t>(result.rows[0][0]), std::int64_t{42});
  EXPECT_EQ(std::get<std::string>(result.rows[0][1]), std::string("MattSQL"));
}

/// Verifies SQL string escaping follows parser semantics.
TEST_CASE(decodes_escaped_string_literals) {
  mattsql::Engine engine;

  const auto result = engine.execute("SELECT 'Matt''SQL' AS name;");

  EXPECT_EQ(result.columns.size(), 1U);
  EXPECT_EQ(result.rows.size(), 1U);
  EXPECT_EQ(result.columns[0], std::string("name"));
  EXPECT_EQ(std::get<std::string>(result.rows[0][0]), std::string("Matt'SQL"));
}

/// Verifies aliases are parsed as SQL, not by raw substring searches.
TEST_CASE(handles_as_inside_string_literals) {
  mattsql::Engine engine;

  const auto result = engine.execute("SELECT 'x as y' AS value;");

  EXPECT_EQ(result.columns.size(), 1U);
  EXPECT_EQ(result.rows.size(), 1U);
  EXPECT_EQ(result.columns[0], std::string("value"));
  EXPECT_EQ(std::get<std::string>(result.rows[0][0]), std::string("x as y"));
}

/// Verifies the hosted phase supports in-memory CREATE, INSERT, and SELECT.
TEST_CASE(executes_in_memory_table_queries) {
  mattsql::Engine engine;

  engine.execute("CREATE TABLE users (id INT, name TEXT, active BOOL);");
  engine.execute("INSERT INTO users VALUES (1, 'Ada', 1);");
  engine.execute("INSERT INTO users VALUES (2, 'Grace', 0);");

  const auto result =
      engine.execute("SELECT id, name FROM users WHERE active = 1;");

  EXPECT_EQ(result.columns.size(), 2U);
  EXPECT_EQ(result.rows.size(), 1U);
  EXPECT_EQ(result.columns[0], std::string("id"));
  EXPECT_EQ(result.columns[1], std::string("name"));
  EXPECT_EQ(std::get<std::int64_t>(result.rows[0][0]), std::int64_t{1});
  EXPECT_EQ(std::get<std::string>(result.rows[0][1]), std::string("Ada"));
}

/// Verifies FROM clauses are bound against known in-memory tables.
TEST_CASE(rejects_unknown_table) {
  mattsql::Engine engine;

  EXPECT_THROWS(engine.execute("SELECT 1 AS one FROM missing;"));
}

/// Verifies unsupported statement kinds fail instead of being accepted.
TEST_CASE(rejects_unsupported_statement) {
  mattsql::Engine engine;

  EXPECT_THROWS(engine.execute("DROP TABLE users;"));
}

/// Verifies unterminated string literals are rejected.
TEST_CASE(rejects_unclosed_string_literal) {
  mattsql::Engine engine;

  EXPECT_THROWS(engine.execute("SELECT 'missing end;"));
}

/// Runs the engine test suite.
int main() { return test::Registry::instance().run(); }
