#include "mattsql/binder/default_binder.hpp"
#include "mattsql/catalog/in_memory_catalog.hpp"
#include "mattsql/common/result_utils.hpp"
#include "mattsql/lexer/lexer.hpp"
#include "mattsql/parser/parser.hpp"

#include "test_framework.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <variant>

namespace {

mattsql::StatementPtr parse_statement(const std::string &sql) {
  mattsql::Lexer lexer(sql);
  mattsql::Parser parser(lexer.Tokenize());
  return parser.ParseStatement();
}

template <typename T, typename Base> const T *as(const Base *node) {
  const auto *value = dynamic_cast<const T *>(node);
  EXPECT_TRUE(value != nullptr);
  return value;
}

mattsql::CreateTableRequest users_request() {
  mattsql::CreateTableRequest request;
  request.name = "users";
  request.schema.columns.push_back({"id", mattsql::SqlType::Integer, false});
  request.schema.columns.push_back({"name", mattsql::SqlType::Text});
  request.schema.columns.push_back({"active", mattsql::SqlType::Boolean, false});
  return request;
}

mattsql::InMemoryCatalog make_catalog() {
  mattsql::InMemoryCatalog catalog;
  const auto created = catalog.CreateTable(users_request());
  EXPECT_TRUE(mattsql::status_ok(created.status));
  return catalog;
}

mattsql::Result<mattsql::BoundStatementPtr> bind_sql(const std::string &sql,
                                                     mattsql::Catalog &catalog) {
  const auto statement = parse_statement(sql);
  mattsql::DefaultBinder binder;
  return binder.Bind(*statement, catalog);
}

} // namespace

/// Verifies CREATE TABLE binding produces a catalog-ready schema request.
TEST_CASE(binder_binds_create_table_request) {
  mattsql::InMemoryCatalog catalog;

  const auto result =
      bind_sql("CREATE TABLE projects (id INT, name TEXT, active BOOL);", catalog);

  EXPECT_TRUE(mattsql::status_ok(result.status));
  const auto *create = as<mattsql::BoundCreateTableStatement>(result.value->get());
  EXPECT_TRUE(create->kind == mattsql::BoundStatementKind::CreateTable);
  EXPECT_EQ(create->request.name, std::string("projects"));
  EXPECT_EQ(create->request.schema.columns.size(), 3U);
  EXPECT_EQ(create->request.schema.columns[0].name, std::string("id"));
  EXPECT_TRUE(create->request.schema.columns[0].type == mattsql::SqlType::Integer);
  EXPECT_EQ(create->request.schema.columns[0].id, mattsql::ColumnId{0});
  EXPECT_TRUE(create->request.schema.columns[1].type == mattsql::SqlType::Text);
  EXPECT_EQ(create->request.schema.columns[1].id, mattsql::ColumnId{1});
  EXPECT_TRUE(create->request.schema.columns[2].type == mattsql::SqlType::Boolean);
  EXPECT_EQ(create->request.schema.columns[2].id, mattsql::ColumnId{2});
}

/// Verifies INSERT binding checks value count and coerces boolean 0/1 literals.
TEST_CASE(binder_binds_insert_values_against_table_schema) {
  auto catalog = make_catalog();

  const auto result = bind_sql("INSERT INTO users VALUES (1, 'Ada', 1);", catalog);

  EXPECT_TRUE(mattsql::status_ok(result.status));
  const auto *insert = as<mattsql::BoundInsertStatement>(result.value->get());
  EXPECT_TRUE(insert->kind == mattsql::BoundStatementKind::Insert);
  EXPECT_EQ(insert->table.id, mattsql::TableId{1});
  EXPECT_EQ(insert->values.size(), 3U);

  const auto *id = as<mattsql::BoundLiteralExpression>(insert->values[0].get());
  EXPECT_TRUE(id->type == mattsql::SqlType::Integer);
  EXPECT_EQ(std::get<std::int64_t>(id->value), std::int64_t{1});

  const auto *name = as<mattsql::BoundLiteralExpression>(insert->values[1].get());
  EXPECT_TRUE(name->type == mattsql::SqlType::Text);
  EXPECT_EQ(std::get<std::string>(name->value), std::string("Ada"));

  const auto *active = as<mattsql::BoundLiteralExpression>(insert->values[2].get());
  EXPECT_TRUE(active->type == mattsql::SqlType::Boolean);
  EXPECT_EQ(std::get<bool>(active->value), true);
}

/// Verifies TRUE/FALSE literals bind as BOOLEAN without integer coercion.
TEST_CASE(binder_binds_boolean_literals) {
  auto catalog = make_catalog();

  const auto insert_result =
      bind_sql("INSERT INTO users VALUES (1, 'Ada', TRUE);", catalog);
  EXPECT_TRUE(mattsql::status_ok(insert_result.status));
  const auto *insert = as<mattsql::BoundInsertStatement>(insert_result.value->get());
  const auto *active = as<mattsql::BoundLiteralExpression>(insert->values[2].get());
  EXPECT_TRUE(active->type == mattsql::SqlType::Boolean);
  EXPECT_EQ(std::get<bool>(active->value), true);

  mattsql::InMemoryCatalog empty_catalog;
  const auto select_result = bind_sql("SELECT false;", empty_catalog);
  EXPECT_TRUE(mattsql::status_ok(select_result.status));
  const auto *select = as<mattsql::BoundSelectStatement>(select_result.value->get());
  const auto *literal =
      as<mattsql::BoundLiteralExpression>(select->projections[0].get());
  EXPECT_TRUE(literal->type == mattsql::SqlType::Boolean);
  EXPECT_EQ(std::get<bool>(literal->value), false);
}

/// Verifies SELECT binding resolves columns, qualifiers, and WHERE types.
TEST_CASE(binder_binds_select_columns_and_where_predicate) {
  auto catalog = make_catalog();

  const auto result =
      bind_sql("SELECT users.id, name FROM USERS WHERE active = 1;", catalog);

  EXPECT_TRUE(mattsql::status_ok(result.status));
  const auto *select = as<mattsql::BoundSelectStatement>(result.value->get());
  EXPECT_TRUE(select->kind == mattsql::BoundStatementKind::Select);
  EXPECT_EQ(select->table.name, std::string("users"));
  EXPECT_EQ(select->projections.size(), 2U);

  const auto *id = as<mattsql::BoundColumnExpression>(select->projections[0].get());
  EXPECT_EQ(id->table_id, mattsql::TableId{1});
  EXPECT_EQ(id->column_id, mattsql::ColumnId{0});
  EXPECT_EQ(id->column_name, std::string("id"));
  EXPECT_TRUE(id->type == mattsql::SqlType::Integer);

  const auto *name = as<mattsql::BoundColumnExpression>(select->projections[1].get());
  EXPECT_EQ(name->column_id, mattsql::ColumnId{1});
  EXPECT_TRUE(name->type == mattsql::SqlType::Text);

  const auto *where = as<mattsql::BoundBinaryExpression>(select->where.get());
  EXPECT_TRUE(where->type == mattsql::SqlType::Boolean);
  EXPECT_TRUE(where->op == mattsql::BinaryOperator::Equal);
  EXPECT_TRUE(where->left->type == mattsql::SqlType::Boolean);
  EXPECT_TRUE(where->right->type == mattsql::SqlType::Integer);
}

/// Verifies SELECT * expands to concrete bound columns during binding.
TEST_CASE(binder_expands_select_star) {
  auto catalog = make_catalog();

  const auto result = bind_sql("SELECT * FROM users;", catalog);

  EXPECT_TRUE(mattsql::status_ok(result.status));
  const auto *select = as<mattsql::BoundSelectStatement>(result.value->get());
  EXPECT_EQ(select->projections.size(), 3U);
  EXPECT_EQ(
      as<mattsql::BoundColumnExpression>(select->projections[0].get())->column_name,
      std::string("id"));
  EXPECT_EQ(
      as<mattsql::BoundColumnExpression>(select->projections[1].get())->column_name,
      std::string("name"));
  EXPECT_EQ(
      as<mattsql::BoundColumnExpression>(select->projections[2].get())->column_name,
      std::string("active"));
}

/// Verifies scalar SELECT statements without FROM bind literal expressions.
TEST_CASE(binder_binds_scalar_select_without_table) {
  mattsql::InMemoryCatalog catalog;

  const auto result = bind_sql("SELECT 1 + 2 AS total, 'x';", catalog);

  EXPECT_TRUE(mattsql::status_ok(result.status));
  const auto *select = as<mattsql::BoundSelectStatement>(result.value->get());
  EXPECT_EQ(select->projections.size(), 2U);
  const auto *sum = as<mattsql::BoundBinaryExpression>(select->projections[0].get());
  EXPECT_TRUE(sum->type == mattsql::SqlType::Integer);
  EXPECT_TRUE(sum->op == mattsql::BinaryOperator::Add);
  EXPECT_TRUE(select->projections[1]->type == mattsql::SqlType::Text);
  EXPECT_TRUE(select->where == nullptr);
}

/// Verifies invalid CREATE TABLE statements fail before catalog mutation.
TEST_CASE(binder_rejects_invalid_create_table_statements) {
  auto catalog = make_catalog();

  EXPECT_TRUE(bind_sql("CREATE TABLE users (id INT);", catalog).status.code ==
              mattsql::ErrorCode::AlreadyExists);
  EXPECT_TRUE(bind_sql("CREATE TABLE dup (id INT, ID TEXT);", catalog).status.code ==
              mattsql::ErrorCode::BindError);

  mattsql::CreateTableStatement empty_columns;
  empty_columns.table_name = "empty";
  mattsql::DefaultBinder binder;
  EXPECT_TRUE(binder.Bind(empty_columns, catalog).status.code ==
              mattsql::ErrorCode::BindError);

  mattsql::CreateTableStatement empty_column_name;
  empty_column_name.table_name = "bad_column";
  empty_column_name.columns.push_back({"", mattsql::TypeName::Int});
  EXPECT_TRUE(binder.Bind(empty_column_name, catalog).status.code ==
              mattsql::ErrorCode::BindError);
}

/// Verifies invalid INSERT statements reject missing tables and bad values.
TEST_CASE(binder_rejects_invalid_insert_statements) {
  auto catalog = make_catalog();

  EXPECT_TRUE(bind_sql("INSERT INTO missing VALUES (1);", catalog).status.code ==
              mattsql::ErrorCode::NotFound);
  EXPECT_TRUE(bind_sql("INSERT INTO users VALUES (1, 'Ada');", catalog).status.code ==
              mattsql::ErrorCode::BindError);
  EXPECT_TRUE(
      bind_sql("INSERT INTO users VALUES ('bad', 'Ada', 1);", catalog).status.code ==
      mattsql::ErrorCode::TypeMismatch);
  EXPECT_TRUE(
      bind_sql("INSERT INTO users VALUES (NULL, 'Ada', 1);", catalog).status.code ==
      mattsql::ErrorCode::TypeMismatch);
  EXPECT_TRUE(
      bind_sql("INSERT INTO users VALUES (id, 'Ada', 1);", catalog).status.code ==
      mattsql::ErrorCode::BindError);
  EXPECT_TRUE(
      bind_sql("INSERT INTO users VALUES (1, 'Ada', 2);", catalog).status.code ==
      mattsql::ErrorCode::TypeMismatch);
}

/// Verifies invalid SELECT statements reject bad names and scalar type errors.
TEST_CASE(binder_rejects_invalid_select_statements) {
  auto catalog = make_catalog();

  EXPECT_TRUE(bind_sql("SELECT id FROM missing;", catalog).status.code ==
              mattsql::ErrorCode::NotFound);
  EXPECT_TRUE(bind_sql("SELECT missing FROM users;", catalog).status.code ==
              mattsql::ErrorCode::BindError);
  EXPECT_TRUE(bind_sql("SELECT id;", catalog).status.code ==
              mattsql::ErrorCode::BindError);
  EXPECT_TRUE(bind_sql("SELECT *;", catalog).status.code ==
              mattsql::ErrorCode::BindError);
  EXPECT_TRUE(bind_sql("SELECT projects.id FROM users;", catalog).status.code ==
              mattsql::ErrorCode::BindError);
  EXPECT_TRUE(bind_sql("SELECT users.id.extra FROM users;", catalog).status.code ==
              mattsql::ErrorCode::BindError);
  EXPECT_TRUE(bind_sql("SELECT name + 1 FROM users;", catalog).status.code ==
              mattsql::ErrorCode::TypeMismatch);
  EXPECT_TRUE(bind_sql("SELECT id FROM users WHERE name;", catalog).status.code ==
              mattsql::ErrorCode::TypeMismatch);
  EXPECT_TRUE(bind_sql("SELECT id FROM users WHERE active = 2;", catalog).status.code ==
              mattsql::ErrorCode::TypeMismatch);
  EXPECT_TRUE(bind_sql("SELECT id FROM users WHERE id < NULL;", catalog).status.code ==
              mattsql::ErrorCode::TypeMismatch);
}
