#include "mattsql/binder/default_binder.hpp"
#include "mattsql/catalog/in_memory_catalog.hpp"
#include "mattsql/common/result_utils.hpp"
#include "mattsql/planner/default_physical_planner.hpp"

#include "sql_pipeline_test_utils.hpp"
#include "test_framework.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <variant>

namespace {

using test::as;
using test::make_catalog;
using test::make_integer_literal;
using test::optimized_physical_plan_sql;
using test::physical_plan_sql;
using test::users_request;

mattsql::TableInfo table_info_with_root(mattsql::PageId root_page_id) {
  mattsql::TableInfo table;
  table.id = 42;
  table.name = "users";
  table.heap_root_page_id = root_page_id;
  table.schema = users_request().schema;
  return table;
}

class LyingLogicalValues final : public mattsql::LogicalPlan {
public:
  [[nodiscard]] mattsql::LogicalOperatorKind Kind() const override {
    return mattsql::LogicalOperatorKind::Values;
  }
};

} // namespace

/// Verifies CREATE TABLE lowers to a physical DDL node with heap storage intent.
TEST_CASE(physical_planner_plans_create_table_storage_intent) {
  mattsql::InMemoryCatalog catalog;

  const auto physical =
      physical_plan_sql("CREATE TABLE projects (id INT, name TEXT);", catalog);

  EXPECT_TRUE(mattsql::status_ok(physical.status));
  const auto *create = as<mattsql::PhysicalCreateTable>(physical.value->get());
  EXPECT_TRUE(create->Kind() == mattsql::PhysicalOperatorKind::CreateTable);
  EXPECT_EQ(create->children.size(), 0U);
  EXPECT_EQ(create->request.name, std::string("projects"));
  EXPECT_TRUE(create->storage_method == mattsql::TableStorageMethod::Heap);
}

/// Verifies INSERT lowers to physical Insert over Values and keeps row payloads.
TEST_CASE(physical_planner_plans_insert_with_heap_storage_reference) {
  auto catalog = make_catalog();

  const auto physical =
      physical_plan_sql("INSERT INTO users VALUES (1, 'Ada', 1);", catalog);

  EXPECT_TRUE(mattsql::status_ok(physical.status));
  const auto *insert = as<mattsql::PhysicalInsert>(physical.value->get());
  EXPECT_TRUE(insert->Kind() == mattsql::PhysicalOperatorKind::Insert);
  EXPECT_EQ(insert->table.name, std::string("users"));
  EXPECT_TRUE(insert->storage.method == mattsql::TableStorageMethod::Heap);
  EXPECT_EQ(insert->storage.table_id, mattsql::TableId{1});
  EXPECT_EQ(insert->storage.table_name, std::string("users"));
  EXPECT_EQ(insert->storage.root_page_id, mattsql::kInvalidPageId);
  EXPECT_EQ(insert->storage.schema.columns.size(), 3U);
  EXPECT_EQ(insert->children.size(), 1U);

  const auto *values = as<mattsql::PhysicalValues>(insert->children[0].get());
  EXPECT_EQ(values->rows.size(), 1U);
  EXPECT_EQ(values->rows[0].size(), 3U);
  EXPECT_TRUE(values->rows[0][2]->type == mattsql::SqlType::Boolean);
}

/// Verifies SELECT lowers to Projection over Filter over heap SeqScan.
TEST_CASE(physical_planner_plans_select_filter_scan) {
  auto catalog = make_catalog();

  const auto physical =
      physical_plan_sql("SELECT id AS user_id FROM users WHERE active = 1;", catalog);

  EXPECT_TRUE(mattsql::status_ok(physical.status));
  const auto *projection = as<mattsql::PhysicalProjection>(physical.value->get());
  EXPECT_TRUE(projection->Kind() == mattsql::PhysicalOperatorKind::Projection);
  EXPECT_EQ(projection->projections.size(), 1U);
  EXPECT_EQ(projection->projection_names.size(), 1U);
  EXPECT_EQ(projection->projection_names[0], std::string("user_id"));
  EXPECT_EQ(projection->children.size(), 1U);

  const auto *filter = as<mattsql::PhysicalFilter>(projection->children[0].get());
  EXPECT_TRUE(filter->predicate->type == mattsql::SqlType::Boolean);
  EXPECT_EQ(filter->children.size(), 1U);

  const auto *scan = as<mattsql::PhysicalSeqScan>(filter->children[0].get());
  EXPECT_TRUE(scan->Kind() == mattsql::PhysicalOperatorKind::SeqScan);
  EXPECT_EQ(scan->storage.table_name, std::string("users"));
  EXPECT_TRUE(scan->storage.method == mattsql::TableStorageMethod::Heap);
}

/// Verifies scalar SELECT lowers to Projection over a one-row Values source.
TEST_CASE(physical_planner_plans_scalar_select_values) {
  mattsql::InMemoryCatalog catalog;

  const auto physical = physical_plan_sql("SELECT 1 + 2;", catalog);

  EXPECT_TRUE(mattsql::status_ok(physical.status));
  const auto *projection = as<mattsql::PhysicalProjection>(physical.value->get());
  EXPECT_EQ(projection->children.size(), 1U);
  const auto *values = as<mattsql::PhysicalValues>(projection->children[0].get());
  EXPECT_EQ(values->rows.size(), 1U);
  EXPECT_EQ(values->rows[0].size(), 0U);
}

/// Verifies optimized constant-false filters become empty physical Values.
TEST_CASE(physical_planner_plans_optimized_empty_input) {
  auto catalog = make_catalog();

  const auto physical =
      optimized_physical_plan_sql("SELECT id FROM users WHERE 1 = 0;", catalog);

  EXPECT_TRUE(mattsql::status_ok(physical.status));
  const auto *projection = as<mattsql::PhysicalProjection>(physical.value->get());
  const auto *values = as<mattsql::PhysicalValues>(projection->children[0].get());
  EXPECT_EQ(values->rows.size(), 0U);
}

/// Verifies table storage references preserve table heap root page identifiers.
TEST_CASE(physical_planner_preserves_heap_root_page_reference) {
  mattsql::LogicalSeqScan logical;
  logical.table = table_info_with_root(mattsql::PageId{99});

  mattsql::DefaultPhysicalPlanner planner;
  const auto physical = planner.Plan(logical);

  EXPECT_TRUE(mattsql::status_ok(physical.status));
  const auto *scan = as<mattsql::PhysicalSeqScan>(physical.value->get());
  EXPECT_EQ(scan->storage.table_id, mattsql::TableId{42});
  EXPECT_EQ(scan->storage.root_page_id, mattsql::PageId{99});
  EXPECT_EQ(scan->storage.schema.columns.size(), 3U);
}

/// Verifies unsupported and malformed logical plan nodes are rejected.
TEST_CASE(physical_planner_rejects_invalid_logical_nodes) {
  mattsql::DefaultPhysicalPlanner planner;

  struct UnknownPlan final : mattsql::LogicalPlan {};
  UnknownPlan unknown;
  EXPECT_TRUE(planner.Plan(unknown).status.code == mattsql::ErrorCode::PlanError);

  LyingLogicalValues lying_values;
  EXPECT_TRUE(planner.Plan(lying_values).status.code ==
              mattsql::ErrorCode::PlanError);

  mattsql::LogicalSeqScan missing_table;
  EXPECT_TRUE(planner.Plan(missing_table).status.code == mattsql::ErrorCode::PlanError);

  mattsql::LogicalProjection missing_child;
  missing_child.projections.push_back(make_integer_literal(1));
  EXPECT_TRUE(planner.Plan(missing_child).status.code == mattsql::ErrorCode::PlanError);

  mattsql::LogicalProjection empty_projection;
  empty_projection.children.push_back(std::make_unique<mattsql::LogicalValues>());
  EXPECT_TRUE(planner.Plan(empty_projection).status.code ==
              mattsql::ErrorCode::PlanError);

  mattsql::LogicalProjection null_projection;
  null_projection.projections.push_back(nullptr);
  null_projection.children.push_back(std::make_unique<mattsql::LogicalValues>());
  EXPECT_TRUE(planner.Plan(null_projection).status.code ==
              mattsql::ErrorCode::PlanError);
}

/// Verifies physical planner catches invalid expressions and filter predicates.
TEST_CASE(physical_planner_rejects_invalid_expressions) {
  mattsql::DefaultPhysicalPlanner planner;

  mattsql::LogicalFilter non_boolean_filter;
  non_boolean_filter.predicate = make_integer_literal(1);
  non_boolean_filter.children.push_back(std::make_unique<mattsql::LogicalValues>());
  EXPECT_TRUE(planner.Plan(non_boolean_filter).status.code ==
              mattsql::ErrorCode::TypeMismatch);

  mattsql::LogicalFilter missing_predicate;
  missing_predicate.children.push_back(std::make_unique<mattsql::LogicalValues>());
  EXPECT_TRUE(planner.Plan(missing_predicate).status.code ==
              mattsql::ErrorCode::PlanError);

  mattsql::LogicalProjection unexpanded_star;
  auto star = std::make_unique<mattsql::BoundStarExpression>();
  unexpanded_star.projections.push_back(std::move(star));
  unexpanded_star.children.push_back(std::make_unique<mattsql::LogicalValues>());
  EXPECT_TRUE(planner.Plan(unexpanded_star).status.code ==
              mattsql::ErrorCode::PlanError);

  mattsql::LogicalProjection malformed_unary;
  auto unary = std::make_unique<mattsql::BoundUnaryExpression>();
  unary->type = mattsql::SqlType::Boolean;
  unary->op = mattsql::UnaryOperator::Not;
  malformed_unary.projections.push_back(std::move(unary));
  malformed_unary.children.push_back(std::make_unique<mattsql::LogicalValues>());
  EXPECT_TRUE(planner.Plan(malformed_unary).status.code ==
              mattsql::ErrorCode::PlanError);
}

/// Verifies invalid physical INSERT lowering inputs are rejected.
TEST_CASE(physical_planner_rejects_invalid_insert_nodes) {
  mattsql::DefaultPhysicalPlanner planner;

  mattsql::LogicalInsert missing_table;
  missing_table.children.push_back(std::make_unique<mattsql::LogicalValues>());
  EXPECT_TRUE(planner.Plan(missing_table).status.code == mattsql::ErrorCode::PlanError);

  mattsql::LogicalInsert missing_values;
  missing_values.table = table_info_with_root(mattsql::PageId{7});
  EXPECT_TRUE(planner.Plan(missing_values).status.code ==
              mattsql::ErrorCode::PlanError);

  mattsql::LogicalValues bad_values;
  auto star = std::make_unique<mattsql::BoundStarExpression>();
  bad_values.rows.push_back({});
  bad_values.rows[0].push_back(std::move(star));
  EXPECT_TRUE(planner.Plan(bad_values).status.code == mattsql::ErrorCode::PlanError);
}
