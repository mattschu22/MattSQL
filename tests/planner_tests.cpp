#include "mattsql/binder/default_binder.hpp"
#include "mattsql/catalog/in_memory_catalog.hpp"
#include "mattsql/common/result_utils.hpp"
#include "mattsql/planner/default_logical_planner.hpp"

#include "sql_pipeline_test_utils.hpp"
#include "test_framework.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace {

using test::as;
using test::bind_sql;
using test::logical_plan_sql;
using test::make_boolean_literal;
using test::make_catalog;
using test::make_column_ref;
using test::make_integer_literal;

class LyingSelectStatement final : public mattsql::BoundStatement {
public:
  [[nodiscard]] mattsql::BoundStatementKind Kind() const override {
    return mattsql::BoundStatementKind::Select;
  }

  std::vector<mattsql::BoundExpressionPtr> projections;
  std::vector<std::string> projection_names;
  mattsql::TableInfo table;
  mattsql::BoundExpressionPtr where;
};

class LyingLiteralExpression final : public mattsql::BoundExpression {
public:
  LyingLiteralExpression() {
    type = mattsql::SqlType::Integer;
    value = std::int64_t{1};
  }

  [[nodiscard]] mattsql::BoundExpressionKind Kind() const override {
    return mattsql::BoundExpressionKind::Literal;
  }

  mattsql::Value value;
};

} // namespace

/// Verifies CREATE TABLE lowers to a leaf logical DDL node.
TEST_CASE(planner_plans_create_table_leaf) {
  mattsql::InMemoryCatalog catalog;

  const auto plan =
      logical_plan_sql("CREATE TABLE projects (id INT, name TEXT);", catalog);

  EXPECT_TRUE(mattsql::status_ok(plan.status));
  const auto *create = as<mattsql::LogicalCreateTable>(plan.value->get());
  EXPECT_TRUE(create->Kind() == mattsql::LogicalOperatorKind::CreateTable);
  EXPECT_EQ(create->children.size(), 0U);
  EXPECT_EQ(create->request.name, std::string("projects"));
  EXPECT_EQ(create->request.schema.columns.size(), 2U);
  EXPECT_TRUE(create->request.schema.columns[0].type == mattsql::SqlType::Integer);
}

/// Verifies INSERT lowers to Insert over Values and keeps bound row payloads.
TEST_CASE(planner_plans_insert_over_values) {
  auto catalog = make_catalog();

  const auto plan =
      logical_plan_sql("INSERT INTO users VALUES (1, 'Ada', 1);", catalog);

  EXPECT_TRUE(mattsql::status_ok(plan.status));
  const auto *insert = as<mattsql::LogicalInsert>(plan.value->get());
  EXPECT_TRUE(insert->Kind() == mattsql::LogicalOperatorKind::Insert);
  EXPECT_EQ(insert->table.name, std::string("users"));
  EXPECT_EQ(insert->children.size(), 1U);

  const auto *values = as<mattsql::LogicalValues>(insert->children[0].get());
  EXPECT_TRUE(values->Kind() == mattsql::LogicalOperatorKind::Values);
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
  EXPECT_TRUE(projection->Kind() == mattsql::LogicalOperatorKind::Projection);
  EXPECT_EQ(projection->projections.size(), 2U);
  EXPECT_EQ(projection->projection_names.size(), 2U);
  EXPECT_EQ(projection->projection_names[0], std::string("user_id"));
  EXPECT_EQ(projection->projection_names[1], std::string(""));
  EXPECT_TRUE(projection->projections[0].get() != bound_select->projections[0].get());
  EXPECT_EQ(projection->children.size(), 1U);

  const auto *filter = as<mattsql::LogicalFilter>(projection->children[0].get());
  EXPECT_TRUE(filter->Kind() == mattsql::LogicalOperatorKind::Filter);
  EXPECT_TRUE(filter->predicate->type == mattsql::SqlType::Boolean);
  EXPECT_TRUE(filter->predicate.get() != bound_select->where.get());
  EXPECT_EQ(filter->children.size(), 1U);

  const auto *scan = as<mattsql::LogicalSeqScan>(filter->children[0].get());
  EXPECT_TRUE(scan->Kind() == mattsql::LogicalOperatorKind::SeqScan);
  EXPECT_EQ(scan->table.name, std::string("users"));
}

/// Verifies unfiltered SELECT lowers directly to Projection over SeqScan.
TEST_CASE(planner_plans_unfiltered_select_scan) {
  auto catalog = make_catalog();

  const auto plan = logical_plan_sql("SELECT id FROM users;", catalog);

  EXPECT_TRUE(mattsql::status_ok(plan.status));
  const auto *projection = as<mattsql::LogicalProjection>(plan.value->get());
  EXPECT_EQ(projection->children.size(), 1U);
  const auto *scan = as<mattsql::LogicalSeqScan>(projection->children[0].get());
  EXPECT_EQ(scan->table.schema.columns.size(), 3U);
}

/// Verifies scalar SELECT uses a single empty Values row as its input source.
TEST_CASE(planner_plans_scalar_select_over_single_values_row) {
  mattsql::InMemoryCatalog catalog;

  const auto plan = logical_plan_sql("SELECT 1 + 2, 'x';", catalog);

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
  EXPECT_TRUE(planner.Plan(empty_select).status.code == mattsql::ErrorCode::PlanError);

  mattsql::BoundSelectStatement null_projection;
  null_projection.projections.push_back(nullptr);
  EXPECT_TRUE(planner.Plan(null_projection).status.code ==
              mattsql::ErrorCode::PlanError);

  mattsql::BoundSelectStatement scalar_column;
  scalar_column.projections.push_back(make_column_ref("id"));
  EXPECT_TRUE(planner.Plan(scalar_column).status.code == mattsql::ErrorCode::PlanError);

  mattsql::BoundSelectStatement unexpanded_star;
  unexpanded_star.table.name = "users";
  auto star = std::make_unique<mattsql::BoundStarExpression>();
  unexpanded_star.projections.push_back(std::move(star));
  EXPECT_TRUE(planner.Plan(unexpanded_star).status.code ==
              mattsql::ErrorCode::PlanError);

  mattsql::BoundInsertStatement missing_target;
  EXPECT_TRUE(planner.Plan(missing_target).status.code ==
              mattsql::ErrorCode::PlanError);

  mattsql::BoundCreateTableStatement missing_name;
  missing_name.request.schema.columns.push_back(
      {"id", mattsql::SqlType::Integer, false});
  EXPECT_TRUE(planner.Plan(missing_name).status.code == mattsql::ErrorCode::PlanError);

  mattsql::BoundCreateTableStatement missing_columns;
  missing_columns.request.name = "empty";
  EXPECT_TRUE(planner.Plan(missing_columns).status.code ==
              mattsql::ErrorCode::PlanError);
}

/// Verifies bound statement dispatch checks runtime type, not just reported Kind.
TEST_CASE(planner_rejects_misreported_bound_statement_kind) {
  mattsql::DefaultLogicalPlanner planner;

  LyingSelectStatement statement;
  statement.projections.push_back(make_integer_literal(1));

  EXPECT_TRUE(planner.Plan(statement).status.code == mattsql::ErrorCode::PlanError);
}

/// Verifies the planner validates operator invariants on bound statements.
TEST_CASE(planner_rejects_invalid_logical_operator_inputs) {
  mattsql::DefaultLogicalPlanner planner;

  mattsql::BoundSelectStatement non_boolean_filter;
  non_boolean_filter.projections.push_back(make_integer_literal(1));
  non_boolean_filter.where = make_integer_literal(1);
  EXPECT_TRUE(planner.Plan(non_boolean_filter).status.code ==
              mattsql::ErrorCode::TypeMismatch);

  mattsql::BoundInsertStatement count_mismatch;
  count_mismatch.table.name = "users";
  count_mismatch.table.schema.columns.push_back(
      {"id", mattsql::SqlType::Integer, false});
  count_mismatch.table.schema.columns.push_back(
      {"active", mattsql::SqlType::Boolean, false});
  count_mismatch.values.push_back(make_integer_literal(1));
  EXPECT_TRUE(planner.Plan(count_mismatch).status.code ==
              mattsql::ErrorCode::PlanError);

  mattsql::BoundInsertStatement null_insert_value;
  null_insert_value.table.name = "users";
  null_insert_value.table.schema.columns.push_back(
      {"id", mattsql::SqlType::Integer, false});
  null_insert_value.values.push_back(nullptr);
  EXPECT_TRUE(planner.Plan(null_insert_value).status.code ==
              mattsql::ErrorCode::PlanError);

  mattsql::BoundInsertStatement column_insert_value;
  column_insert_value.table.name = "users";
  column_insert_value.table.schema.columns.push_back(
      {"id", mattsql::SqlType::Integer, false});
  column_insert_value.values.push_back(make_column_ref("id"));
  EXPECT_TRUE(planner.Plan(column_insert_value).status.code ==
              mattsql::ErrorCode::PlanError);

  mattsql::BoundSelectStatement malformed_unary;
  auto unary = std::make_unique<mattsql::BoundUnaryExpression>();
  unary->type = mattsql::SqlType::Boolean;
  unary->op = mattsql::UnaryOperator::Not;
  malformed_unary.projections.push_back(std::move(unary));
  EXPECT_TRUE(planner.Plan(malformed_unary).status.code ==
              mattsql::ErrorCode::PlanError);

  mattsql::BoundSelectStatement malformed_binary;
  auto binary = std::make_unique<mattsql::BoundBinaryExpression>();
  binary->type = mattsql::SqlType::Boolean;
  binary->left = make_boolean_literal(true);
  binary->op = mattsql::BinaryOperator::And;
  malformed_binary.projections.push_back(std::move(binary));
  EXPECT_TRUE(planner.Plan(malformed_binary).status.code ==
              mattsql::ErrorCode::PlanError);
}

/// Verifies bound expression dispatch checks runtime type, not just reported Kind.
TEST_CASE(planner_rejects_misreported_bound_expression_kind) {
  mattsql::DefaultLogicalPlanner planner;

  mattsql::BoundSelectStatement statement;
  statement.projections.push_back(std::make_unique<LyingLiteralExpression>());

  EXPECT_TRUE(planner.Plan(statement).status.code == mattsql::ErrorCode::PlanError);
}
