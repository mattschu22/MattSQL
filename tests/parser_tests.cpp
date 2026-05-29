#include "mattsql/lexer/lexer.hpp"
#include "mattsql/parser/parser.hpp"

#include "test_framework.hpp"

#include <memory>
#include <string>

namespace {

/// Lexes and parses a SQL statement for parser tests.
mattsql::StatementPtr parse_statement(const std::string &sql) {
  mattsql::Lexer lexer(sql);
  mattsql::Parser parser(lexer.Tokenize());
  return parser.ParseStatement();
}

/// Downcasts a mutable AST node and asserts the cast succeeds.
template <typename T> T *as(mattsql::AstNode *node) {
  auto *value = dynamic_cast<T *>(node);
  EXPECT_TRUE(value != nullptr);
  return value;
}

/// Downcasts a const AST node and asserts the cast succeeds.
template <typename T> const T *as(const mattsql::AstNode *node) {
  const auto *value = dynamic_cast<const T *>(node);
  EXPECT_TRUE(value != nullptr);
  return value;
}

} // namespace

/// Verifies SELECT parsing, FROM parsing, WHERE parsing, and boolean precedence.
TEST_CASE(parses_select_with_from_where_and_precedence) {
  const auto statement =
      parse_statement("SELECT id, name FROM users WHERE age >= 18 AND active = 1;");

  const auto *select = as<mattsql::SelectStatement>(statement.get());
  EXPECT_EQ(select->projections.size(), 2U);
  EXPECT_EQ(select->table_name, std::string("users"));
  EXPECT_TRUE(select->where != nullptr);

  const auto *first_projection =
      as<mattsql::ColumnRef>(select->projections[0].expression.get());
  const auto *second_projection =
      as<mattsql::ColumnRef>(select->projections[1].expression.get());
  EXPECT_EQ(first_projection->name, std::string("id"));
  EXPECT_EQ(second_projection->name, std::string("name"));

  const auto *where = as<mattsql::BinaryExpression>(select->where.get());
  EXPECT_TRUE(where->op == mattsql::BinaryOperator::And);
  EXPECT_TRUE(as<mattsql::BinaryExpression>(where->left.get())->op ==
              mattsql::BinaryOperator::GreaterEqual);
  EXPECT_TRUE(as<mattsql::BinaryExpression>(where->right.get())->op ==
              mattsql::BinaryOperator::Equal);
}

/// Verifies INSERT parsing and SQL string literal decoding.
TEST_CASE(parses_insert_values_and_decodes_strings) {
  const auto statement =
      parse_statement("INSERT INTO users VALUES (1, 'Matt''SQL', NULL);");

  const auto *insert = as<mattsql::InsertStatement>(statement.get());
  EXPECT_EQ(insert->table_name, std::string("users"));
  EXPECT_EQ(insert->values.size(), 3U);

  EXPECT_EQ(as<mattsql::IntegerLiteral>(insert->values[0].get())->value, 1);
  EXPECT_EQ(as<mattsql::StringLiteral>(insert->values[1].get())->value,
            std::string("Matt'SQL"));
  as<mattsql::NullLiteral>(insert->values[2].get());
}

/// Verifies TRUE and FALSE parse as boolean literal expressions.
TEST_CASE(parses_boolean_literals) {
  const auto statement = parse_statement("SELECT TRUE, false;");

  const auto *select = as<mattsql::SelectStatement>(statement.get());
  EXPECT_EQ(select->projections.size(), 2U);
  EXPECT_EQ(as<mattsql::BooleanLiteral>(select->projections[0].expression.get())->value,
            true);
  EXPECT_EQ(as<mattsql::BooleanLiteral>(select->projections[1].expression.get())->value,
            false);
}

/// Verifies CREATE TABLE column names and supported type names.
TEST_CASE(parses_create_table_column_definitions) {
  const auto statement =
      parse_statement("CREATE TABLE users (id INT, name TEXT, active BOOL);");

  const auto *create = as<mattsql::CreateTableStatement>(statement.get());
  EXPECT_EQ(create->table_name, std::string("users"));
  EXPECT_EQ(create->columns.size(), 3U);
  EXPECT_EQ(create->columns[0].name, std::string("id"));
  EXPECT_TRUE(create->columns[0].type == mattsql::TypeName::Int);
  EXPECT_EQ(create->columns[1].name, std::string("name"));
  EXPECT_TRUE(create->columns[1].type == mattsql::TypeName::Text);
  EXPECT_EQ(create->columns[2].name, std::string("active"));
  EXPECT_TRUE(create->columns[2].type == mattsql::TypeName::Bool);
}

/// Verifies multiplication binds tighter than addition.
TEST_CASE(parses_arithmetic_precedence) {
  const auto statement = parse_statement("SELECT 1 + 2 * 3;");

  const auto *select = as<mattsql::SelectStatement>(statement.get());
  const auto *add =
      as<mattsql::BinaryExpression>(select->projections[0].expression.get());
  EXPECT_TRUE(add->op == mattsql::BinaryOperator::Add);
  as<mattsql::IntegerLiteral>(add->left.get());

  const auto *multiply = as<mattsql::BinaryExpression>(add->right.get());
  EXPECT_TRUE(multiply->op == mattsql::BinaryOperator::Multiply);
}

/// Verifies SELECT projection aliases are parsed outside expression text.
TEST_CASE(parses_select_projection_aliases) {
  const auto statement =
      parse_statement("SELECT 'x as y' AS label, id AS user_id FROM users;");

  const auto *select = as<mattsql::SelectStatement>(statement.get());
  EXPECT_EQ(select->projections.size(), 2U);
  EXPECT_EQ(select->projections[0].alias, std::string("label"));
  EXPECT_EQ(select->projections[1].alias, std::string("user_id"));
  EXPECT_EQ(as<mattsql::StringLiteral>(select->projections[0].expression.get())->value,
            std::string("x as y"));
  EXPECT_EQ(as<mattsql::ColumnRef>(select->projections[1].expression.get())->name,
            std::string("id"));
}

/// Verifies parse errors expose useful source locations.
TEST_CASE(reports_parse_errors_with_locations) {
  try {
    parse_statement("SELECT FROM users;");
    EXPECT_TRUE(false);
  } catch (const mattsql::ParseError &error) {
    EXPECT_EQ(error.Location().line, 1U);
    EXPECT_EQ(error.Location().column, 8U);
  }
}
