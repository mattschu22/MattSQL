#include "mattsql/binder/default_binder.hpp"
#include "mattsql/catalog/in_memory_catalog.hpp"
#include "mattsql/common/result_utils.hpp"
#include "mattsql/lexer/lexer.hpp"
#include "mattsql/parser/parser.hpp"
#include "mattsql/planner/default_logical_planner.hpp"

#include "test_framework.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
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

mattsql::Result<mattsql::LogicalPlanPtr> plan_sql(const std::string &sql,
                                                  mattsql::Catalog &catalog) {
  auto bound = bind_sql(sql, catalog);
  if (!mattsql::status_ok(bound.status)) {
    mattsql::Result<mattsql::LogicalPlanPtr> result;
    result.status = bound.status;
    return result;
  }

  mattsql::DefaultLogicalPlanner planner;
  return planner.Plan(**bound.value);
}

mattsql::BoundExpressionPtr make_integer_literal(std::int64_t value) {
  auto literal = std::make_unique<mattsql::BoundLiteralExpression>();
  literal->kind = mattsql::BoundExpressionKind::Literal;
  literal->type = mattsql::SqlType::Integer;
  literal->value = value;
  return literal;
}

mattsql::BoundExpressionPtr make_boolean_literal(bool value) {
  auto literal = std::make_unique<mattsql::BoundLiteralExpression>();
  literal->kind = mattsql::BoundExpressionKind::Literal;
  literal->type = mattsql::SqlType::Boolean;
  literal->value = value;
  return literal;
}

mattsql::BoundExpressionPtr make_column_ref(std::string name) {
  auto column = std::make_unique<mattsql::BoundColumnExpression>();
  column->kind = mattsql::BoundExpressionKind::Column;
  column->type = mattsql::SqlType::Integer;
  column->table_id = 1;
  column->column_id = 0;
  column->table_name = "users";
  column->column_name = std::move(name);
  return column;
}

} // namespace

/// Verifies CREATE TABLE lowers to a leaf logical DDL node.
TEST_CASE(planner_plans_create_table_leaf) {
  mattsql::InMemoryCatalog catalog;

  const auto plan = plan_sql("CREATE TABLE projects (id INT, name TEXT);", catalog);

  EXPECT_TRUE(mattsql::status_ok(plan.status));
  const auto *create = as<mattsql::LogicalCreateTable>(plan.value->get());
  EXPECT_TRUE(create->kind == mattsql::LogicalOperatorKind::CreateTable);
  EXPECT_EQ(create->children.size(), 0U);
  EXPECT_EQ(create->request.name, std::string("projects"));
  EXPECT_EQ(create->request.schema.columns.size(), 2U);
  EXPECT_TRUE(create->request.schema.columns[0].type == mattsql::SqlType::Integer);
}

/// Verifies INSERT lowers to Insert over Values and keeps bound row payloads.
TEST_CASE(planner_plans_insert_over_values) {
  auto catalog = make_catalog();

  const auto plan = plan_sql("INSERT INTO users VALUES (1, 'Ada', 1);", catalog);

  EXPECT_TRUE(mattsql::status_ok(plan.status));
  const auto *insert = as<mattsql::LogicalInsert>(plan.value->get());
  EXPECT_TRUE(insert->kind == mattsql::LogicalOperatorKind::Insert);
  EXPECT_EQ(insert->table.name, std::string("users"));
  EXPECT_EQ(insert->children.size(), 1U);

  const auto *values = as<mattsql::LogicalValues>(insert->children[0].get());
  EXPECT_TRUE(values->kind == mattsql::LogicalOperatorKind::Values);
  EXPECT_EQ(values->rows.size(), 1U);
  EXPECT_EQ(values->rows[0].size(), 3U);
  EXPECT_TRUE(values->rows[0][0]->type == mattsql::SqlType::Integer);
  EXPECT_TRUE(values->rows[0][1]->type == mattsql::SqlType::Text);
  EXPECT_TRUE(values->rows[0][2]->type == mattsql::SqlType::Boolean);

  const auto *active = as<mattsql::BoundLiteralExpression>(values->rows[0][2].get());
  EXPECT_EQ(std::get<bool>(active->value), true);
}

/// Verifies filtered SELECT lowers to Projection over Filter over SeqScan.
TEST_CASE(planner_plans_select_projection_filter_scan) {
  auto catalog = make_catalog();
  auto bound =
      bind_sql("SELECT id AS user_id, name FROM users WHERE active = 1;", catalog);
  EXPECT_TRUE(mattsql::status_ok(bound.status));

  const auto *bound_select = as<mattsql::BoundSelectStatement>(bound.value->get());

  mattsql::DefaultLogicalPlanner planner;
  auto plan = planner.Plan(**bound.value);

  EXPECT_TRUE(mattsql::status_ok(plan.status));
  const auto *projection = as<mattsql::LogicalProjection>(plan.value->get());
  EXPECT_TRUE(projection->kind == mattsql::LogicalOperatorKind::Projection);
  EXPECT_EQ(projection->projections.size(), 2U);
  EXPECT_EQ(projection->projection_names.size(), 2U);
  EXPECT_EQ(projection->projection_names[0], std::string("user_id"));
  EXPECT_EQ(projection->projection_names[1], std::string(""));
  EXPECT_TRUE(projection->projections[0].get() != bound_select->projections[0].get());
  EXPECT_EQ(projection->children.size(), 1U);

  const auto *filter = as<mattsql::LogicalFilter>(projection->children[0].get());
  EXPECT_TRUE(filter->kind == mattsql::LogicalOperatorKind::Filter);
  EXPECT_TRUE(filter->predicate->type == mattsql::SqlType::Boolean);
  EXPECT_TRUE(filter->predicate.get() != bound_select->where.get());
  EXPECT_EQ(filter->children.size(), 1U);

  const auto *scan = as<mattsql::LogicalSeqScan>(filter->children[0].get());
  EXPECT_TRUE(scan->kind == mattsql::LogicalOperatorKind::SeqScan);
  EXPECT_EQ(scan->table.name, std::string("users"));
}

/// Verifies unfiltered SELECT lowers directly to Projection over SeqScan.
TEST_CASE(planner_plans_unfiltered_select_scan) {
  auto catalog = make_catalog();

  const auto plan = plan_sql("SELECT id FROM users;", catalog);

  EXPECT_TRUE(mattsql::status_ok(plan.status));
  const auto *projection = as<mattsql::LogicalProjection>(plan.value->get());
  EXPECT_EQ(projection->children.size(), 1U);
  const auto *scan = as<mattsql::LogicalSeqScan>(projection->children[0].get());
  EXPECT_EQ(scan->table.schema.columns.size(), 3U);
}

/// Verifies scalar SELECT uses a single empty Values row as its input source.
TEST_CASE(planner_plans_scalar_select_over_single_values_row) {
  mattsql::InMemoryCatalog catalog;

  const auto plan = plan_sql("SELECT 1 + 2, 'x';", catalog);

  EXPECT_TRUE(mattsql::status_ok(plan.status));
  const auto *projection = as<mattsql::LogicalProjection>(plan.value->get());
  EXPECT_EQ(projection->projections.size(), 2U);
  EXPECT_TRUE(projection->projections[0]->type == mattsql::SqlType::Integer);
  EXPECT_EQ(projection->children.size(), 1U);

  const auto *values = as<mattsql::LogicalValues>(projection->children[0].get());
  EXPECT_EQ(values->rows.size(), 1U);
  EXPECT_EQ(values->rows[0].size(), 0U);
}

/// Verifies invalid bound statements are rejected before producing a plan.
TEST_CASE(planner_rejects_invalid_bound_statements) {
  mattsql::DefaultLogicalPlanner planner;

  struct UnknownStatement final : mattsql::BoundStatement {};
  UnknownStatement unknown;
  EXPECT_TRUE(planner.Plan(unknown).status.code == mattsql::ErrorCode::PlanError);

  mattsql::BoundSelectStatement empty_select;
  empty_select.kind = mattsql::BoundStatementKind::Select;
  EXPECT_TRUE(planner.Plan(empty_select).status.code == mattsql::ErrorCode::PlanError);

  mattsql::BoundSelectStatement null_projection;
  null_projection.kind = mattsql::BoundStatementKind::Select;
  null_projection.projections.push_back(nullptr);
  EXPECT_TRUE(planner.Plan(null_projection).status.code ==
              mattsql::ErrorCode::PlanError);

  mattsql::BoundSelectStatement scalar_column;
  scalar_column.kind = mattsql::BoundStatementKind::Select;
  scalar_column.projections.push_back(make_column_ref("id"));
  EXPECT_TRUE(planner.Plan(scalar_column).status.code == mattsql::ErrorCode::PlanError);

  mattsql::BoundSelectStatement unexpanded_star;
  unexpanded_star.kind = mattsql::BoundStatementKind::Select;
  unexpanded_star.table.name = "users";
  auto star = std::make_unique<mattsql::BoundStarExpression>();
  star->kind = mattsql::BoundExpressionKind::Star;
  unexpanded_star.projections.push_back(std::move(star));
  EXPECT_TRUE(planner.Plan(unexpanded_star).status.code ==
              mattsql::ErrorCode::PlanError);

  mattsql::BoundInsertStatement missing_target;
  missing_target.kind = mattsql::BoundStatementKind::Insert;
  EXPECT_TRUE(planner.Plan(missing_target).status.code ==
              mattsql::ErrorCode::PlanError);

  mattsql::BoundCreateTableStatement missing_name;
  missing_name.kind = mattsql::BoundStatementKind::CreateTable;
  missing_name.request.schema.columns.push_back(
      {"id", mattsql::SqlType::Integer, false});
  EXPECT_TRUE(planner.Plan(missing_name).status.code == mattsql::ErrorCode::PlanError);

  mattsql::BoundCreateTableStatement missing_columns;
  missing_columns.kind = mattsql::BoundStatementKind::CreateTable;
  missing_columns.request.name = "empty";
  EXPECT_TRUE(planner.Plan(missing_columns).status.code ==
              mattsql::ErrorCode::PlanError);
}

/// Verifies the planner validates operator invariants on bound statements.
TEST_CASE(planner_rejects_invalid_logical_operator_inputs) {
  mattsql::DefaultLogicalPlanner planner;

  mattsql::BoundSelectStatement non_boolean_filter;
  non_boolean_filter.kind = mattsql::BoundStatementKind::Select;
  non_boolean_filter.projections.push_back(make_integer_literal(1));
  non_boolean_filter.where = make_integer_literal(1);
  EXPECT_TRUE(planner.Plan(non_boolean_filter).status.code ==
              mattsql::ErrorCode::TypeMismatch);

  mattsql::BoundInsertStatement count_mismatch;
  count_mismatch.kind = mattsql::BoundStatementKind::Insert;
  count_mismatch.table.name = "users";
  count_mismatch.table.schema.columns.push_back(
      {"id", mattsql::SqlType::Integer, false});
  count_mismatch.table.schema.columns.push_back(
      {"active", mattsql::SqlType::Boolean, false});
  count_mismatch.values.push_back(make_integer_literal(1));
  EXPECT_TRUE(planner.Plan(count_mismatch).status.code ==
              mattsql::ErrorCode::PlanError);

  mattsql::BoundInsertStatement null_insert_value;
  null_insert_value.kind = mattsql::BoundStatementKind::Insert;
  null_insert_value.table.name = "users";
  null_insert_value.table.schema.columns.push_back(
      {"id", mattsql::SqlType::Integer, false});
  null_insert_value.values.push_back(nullptr);
  EXPECT_TRUE(planner.Plan(null_insert_value).status.code ==
              mattsql::ErrorCode::PlanError);

  mattsql::BoundInsertStatement column_insert_value;
  column_insert_value.kind = mattsql::BoundStatementKind::Insert;
  column_insert_value.table.name = "users";
  column_insert_value.table.schema.columns.push_back(
      {"id", mattsql::SqlType::Integer, false});
  column_insert_value.values.push_back(make_column_ref("id"));
  EXPECT_TRUE(planner.Plan(column_insert_value).status.code ==
              mattsql::ErrorCode::PlanError);

  mattsql::BoundSelectStatement malformed_unary;
  malformed_unary.kind = mattsql::BoundStatementKind::Select;
  auto unary = std::make_unique<mattsql::BoundUnaryExpression>();
  unary->kind = mattsql::BoundExpressionKind::Unary;
  unary->type = mattsql::SqlType::Boolean;
  unary->op = mattsql::UnaryOperator::Not;
  malformed_unary.projections.push_back(std::move(unary));
  EXPECT_TRUE(planner.Plan(malformed_unary).status.code ==
              mattsql::ErrorCode::PlanError);

  mattsql::BoundSelectStatement malformed_binary;
  malformed_binary.kind = mattsql::BoundStatementKind::Select;
  auto binary = std::make_unique<mattsql::BoundBinaryExpression>();
  binary->kind = mattsql::BoundExpressionKind::Binary;
  binary->type = mattsql::SqlType::Boolean;
  binary->left = make_boolean_literal(true);
  binary->op = mattsql::BinaryOperator::And;
  malformed_binary.projections.push_back(std::move(binary));
  EXPECT_TRUE(planner.Plan(malformed_binary).status.code ==
              mattsql::ErrorCode::PlanError);
}
