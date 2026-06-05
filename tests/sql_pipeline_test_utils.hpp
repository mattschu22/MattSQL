#pragma once

#include "mattsql/binder/default_binder.hpp"
#include "mattsql/binder/expression_utils.hpp"
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
#include <string_view>
#include <utility>

namespace test {

template <typename T, typename Base> const T *as(const Base *node) {
  const auto *value = dynamic_cast<const T *>(node);
  EXPECT_TRUE(value != nullptr);
  return value;
}

inline mattsql::StatementPtr parse_statement(std::string_view sql) {
  mattsql::Lexer lexer(sql);
  mattsql::Parser parser(lexer.Tokenize());
  return parser.ParseStatement();
}

inline mattsql::CreateTableRequest users_request() {
  mattsql::CreateTableRequest request;
  request.name = "users";
  request.schema.columns.push_back({"id", mattsql::SqlType::Integer, false});
  request.schema.columns.push_back({"name", mattsql::SqlType::Text});
  request.schema.columns.push_back({"active", mattsql::SqlType::Boolean, false});
  return request;
}

inline mattsql::InMemoryCatalog make_catalog() {
  mattsql::InMemoryCatalog catalog;
  const auto created = catalog.CreateTable(users_request());
  EXPECT_TRUE(mattsql::status_ok(created.status));
  return catalog;
}

inline mattsql::Result<mattsql::BoundStatementPtr> bind_sql(std::string_view sql,
                                                            mattsql::Catalog &catalog) {
  const auto statement = parse_statement(sql);
  mattsql::DefaultBinder binder;
  return binder.Bind(*statement, catalog);
}

inline mattsql::Result<mattsql::LogicalPlanPtr>
logical_plan_sql(std::string_view sql, mattsql::Catalog &catalog) {
  auto bound = bind_sql(sql, catalog);
  if (!bound.ok()) {
    return mattsql::error_result<mattsql::LogicalPlanPtr>(std::move(bound.status));
  }

  mattsql::DefaultLogicalPlanner planner;
  return planner.Plan(**bound.value);
}

inline mattsql::Result<mattsql::LogicalPlanPtr>
optimized_logical_plan_sql(std::string_view sql, mattsql::Catalog &catalog) {
  auto logical = logical_plan_sql(sql, catalog);
  if (!logical.ok()) {
    return logical;
  }

  mattsql::DefaultOptimizer optimizer;
  return optimizer.Optimize(std::move(logical).TakeValue());
}

inline mattsql::Result<mattsql::PhysicalPlanPtr>
physical_plan_sql(std::string_view sql, mattsql::Catalog &catalog) {
  auto logical = logical_plan_sql(sql, catalog);
  if (!logical.ok()) {
    return mattsql::error_result<mattsql::PhysicalPlanPtr>(std::move(logical.status));
  }

  mattsql::DefaultPhysicalPlanner planner;
  return planner.Plan(**logical.value);
}

inline mattsql::Result<mattsql::PhysicalPlanPtr>
optimized_physical_plan_sql(std::string_view sql, mattsql::Catalog &catalog) {
  auto logical = optimized_logical_plan_sql(sql, catalog);
  if (!logical.ok()) {
    return mattsql::error_result<mattsql::PhysicalPlanPtr>(std::move(logical.status));
  }

  mattsql::DefaultPhysicalPlanner planner;
  return planner.Plan(**logical.value);
}

inline mattsql::BoundExpressionPtr make_integer_literal(std::int64_t value) {
  return mattsql::MakeBoundLiteral(value, mattsql::SqlType::Integer);
}

inline mattsql::BoundExpressionPtr make_boolean_literal(bool value) {
  return mattsql::MakeBoundLiteral(value, mattsql::SqlType::Boolean);
}

inline mattsql::BoundExpressionPtr make_column_ref(std::string name) {
  mattsql::TableInfo table;
  table.id = 1;
  table.name = "users";

  mattsql::ColumnSchema column;
  column.id = 0;
  column.name = std::move(name);
  column.type = mattsql::SqlType::Integer;
  return mattsql::MakeBoundColumn(table, column);
}

} // namespace test
