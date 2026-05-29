#include "mattsql/binder/default_binder.hpp"
#include "mattsql/catalog/in_memory_catalog.hpp"
#include "mattsql/common/result_utils.hpp"
#include "mattsql/lexer/lexer.hpp"
#include "mattsql/optimizer/default_optimizer.hpp"
#include "mattsql/parser/parser.hpp"
#include "mattsql/planner/default_logical_planner.hpp"
#include "mattsql/planner/default_physical_planner.hpp"

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

mattsql::Result<mattsql::LogicalPlanPtr> logical_plan_sql(const std::string &sql,
                                                          mattsql::Catalog &catalog) {
  const auto statement = parse_statement(sql);

  mattsql::DefaultBinder binder;
  auto bound = binder.Bind(*statement, catalog);
  if (!mattsql::status_ok(bound.status)) {
    return mattsql::error_result<mattsql::LogicalPlanPtr>(bound.status);
  }

  mattsql::DefaultLogicalPlanner planner;
  return planner.Plan(**bound.value);
}

mattsql::Result<mattsql::PhysicalPlanPtr> physical_plan_sql(const std::string &sql,
                                                            mattsql::Catalog &catalog) {
  auto logical = logical_plan_sql(sql, catalog);
  if (!mattsql::status_ok(logical.status)) {
    return mattsql::error_result<mattsql::PhysicalPlanPtr>(logical.status);
  }

  mattsql::DefaultPhysicalPlanner planner;
  return planner.Plan(**logical.value);
}

mattsql::Result<mattsql::PhysicalPlanPtr>
optimized_physical_plan_sql(const std::string &sql, mattsql::Catalog &catalog) {
  auto logical = logical_plan_sql(sql, catalog);
  if (!mattsql::status_ok(logical.status)) {
    return mattsql::error_result<mattsql::PhysicalPlanPtr>(logical.status);
  }

  mattsql::DefaultOptimizer optimizer;
  auto optimized = optimizer.Optimize(std::move(*logical.value));
  if (!mattsql::status_ok(optimized.status)) {
    return mattsql::error_result<mattsql::PhysicalPlanPtr>(optimized.status);
  }

  mattsql::DefaultPhysicalPlanner planner;
  return planner.Plan(**optimized.value);
}

mattsql::BoundExpressionPtr make_integer_literal(std::int64_t value) {
  auto literal = std::make_unique<mattsql::BoundLiteralExpression>();
  literal->kind = mattsql::BoundExpressionKind::Literal;
  literal->type = mattsql::SqlType::Integer;
  literal->value = value;
  return literal;
}

mattsql::TableInfo table_info_with_root(mattsql::PageId root_page_id) {
  mattsql::TableInfo table;
  table.id = 42;
  table.name = "users";
  table.heap_root_page_id = root_page_id;
  table.schema = users_request().schema;
  return table;
}

} // namespace

/// Verifies CREATE TABLE lowers to a physical DDL node with heap storage intent.
TEST_CASE(physical_planner_plans_create_table_storage_intent) {
  mattsql::InMemoryCatalog catalog;

  const auto physical =
      physical_plan_sql("CREATE TABLE projects (id INT, name TEXT);", catalog);

  EXPECT_TRUE(mattsql::status_ok(physical.status));
  const auto *create = as<mattsql::PhysicalCreateTable>(physical.value->get());
  EXPECT_TRUE(create->kind == mattsql::PhysicalOperatorKind::CreateTable);
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
  EXPECT_TRUE(insert->kind == mattsql::PhysicalOperatorKind::Insert);
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
      physical_plan_sql("SELECT id FROM users WHERE active = 1;", catalog);

  EXPECT_TRUE(mattsql::status_ok(physical.status));
  const auto *projection = as<mattsql::PhysicalProjection>(physical.value->get());
  EXPECT_TRUE(projection->kind == mattsql::PhysicalOperatorKind::Projection);
  EXPECT_EQ(projection->projections.size(), 1U);
  EXPECT_EQ(projection->children.size(), 1U);

  const auto *filter = as<mattsql::PhysicalFilter>(projection->children[0].get());
  EXPECT_TRUE(filter->predicate->type == mattsql::SqlType::Boolean);
  EXPECT_EQ(filter->children.size(), 1U);

  const auto *scan = as<mattsql::PhysicalSeqScan>(filter->children[0].get());
  EXPECT_TRUE(scan->kind == mattsql::PhysicalOperatorKind::SeqScan);
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
  logical.kind = mattsql::LogicalOperatorKind::SeqScan;
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

  mattsql::LogicalSeqScan missing_table;
  missing_table.kind = mattsql::LogicalOperatorKind::SeqScan;
  EXPECT_TRUE(planner.Plan(missing_table).status.code == mattsql::ErrorCode::PlanError);

  mattsql::LogicalProjection missing_child;
  missing_child.kind = mattsql::LogicalOperatorKind::Projection;
  missing_child.projections.push_back(make_integer_literal(1));
  EXPECT_TRUE(planner.Plan(missing_child).status.code == mattsql::ErrorCode::PlanError);

  mattsql::LogicalProjection empty_projection;
  empty_projection.kind = mattsql::LogicalOperatorKind::Projection;
  empty_projection.children.push_back(std::make_unique<mattsql::LogicalValues>());
  EXPECT_TRUE(planner.Plan(empty_projection).status.code ==
              mattsql::ErrorCode::PlanError);

  mattsql::LogicalProjection null_projection;
  null_projection.kind = mattsql::LogicalOperatorKind::Projection;
  null_projection.projections.push_back(nullptr);
  null_projection.children.push_back(std::make_unique<mattsql::LogicalValues>());
  EXPECT_TRUE(planner.Plan(null_projection).status.code ==
              mattsql::ErrorCode::PlanError);
}

/// Verifies physical planner catches invalid expressions and filter predicates.
TEST_CASE(physical_planner_rejects_invalid_expressions) {
  mattsql::DefaultPhysicalPlanner planner;

  mattsql::LogicalFilter non_boolean_filter;
  non_boolean_filter.kind = mattsql::LogicalOperatorKind::Filter;
  non_boolean_filter.predicate = make_integer_literal(1);
  non_boolean_filter.children.push_back(std::make_unique<mattsql::LogicalValues>());
  EXPECT_TRUE(planner.Plan(non_boolean_filter).status.code ==
              mattsql::ErrorCode::TypeMismatch);

  mattsql::LogicalFilter missing_predicate;
  missing_predicate.kind = mattsql::LogicalOperatorKind::Filter;
  missing_predicate.children.push_back(std::make_unique<mattsql::LogicalValues>());
  EXPECT_TRUE(planner.Plan(missing_predicate).status.code ==
              mattsql::ErrorCode::PlanError);

  mattsql::LogicalProjection unexpanded_star;
  unexpanded_star.kind = mattsql::LogicalOperatorKind::Projection;
  auto star = std::make_unique<mattsql::BoundStarExpression>();
  star->kind = mattsql::BoundExpressionKind::Star;
  unexpanded_star.projections.push_back(std::move(star));
  unexpanded_star.children.push_back(std::make_unique<mattsql::LogicalValues>());
  EXPECT_TRUE(planner.Plan(unexpanded_star).status.code ==
              mattsql::ErrorCode::PlanError);

  mattsql::LogicalProjection malformed_unary;
  malformed_unary.kind = mattsql::LogicalOperatorKind::Projection;
  auto unary = std::make_unique<mattsql::BoundUnaryExpression>();
  unary->kind = mattsql::BoundExpressionKind::Unary;
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
  missing_table.kind = mattsql::LogicalOperatorKind::Insert;
  missing_table.children.push_back(std::make_unique<mattsql::LogicalValues>());
  EXPECT_TRUE(planner.Plan(missing_table).status.code == mattsql::ErrorCode::PlanError);

  mattsql::LogicalInsert missing_values;
  missing_values.kind = mattsql::LogicalOperatorKind::Insert;
  missing_values.table = table_info_with_root(mattsql::PageId{7});
  EXPECT_TRUE(planner.Plan(missing_values).status.code ==
              mattsql::ErrorCode::PlanError);

  mattsql::LogicalValues bad_values;
  bad_values.kind = mattsql::LogicalOperatorKind::Values;
  auto star = std::make_unique<mattsql::BoundStarExpression>();
  star->kind = mattsql::BoundExpressionKind::Star;
  bad_values.rows.push_back({});
  bad_values.rows[0].push_back(std::move(star));
  EXPECT_TRUE(planner.Plan(bad_values).status.code == mattsql::ErrorCode::PlanError);
}
