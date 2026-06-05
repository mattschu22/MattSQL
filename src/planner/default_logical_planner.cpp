#include "mattsql/planner/default_logical_planner.hpp"

#include "mattsql/binder/expression_utils.hpp"
#include "mattsql/common/result_utils.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mattsql {
namespace {

[[nodiscard]] LogicalPlanPtr make_seq_scan_plan(const TableInfo &table) {
  auto scan = std::make_unique<LogicalSeqScan>();
  scan->table = table;
  return scan;
}

[[nodiscard]] Result<LogicalPlanPtr> make_filter_plan(BoundExpressionPtr predicate,
                                                      LogicalPlanPtr child) {
  if (predicate == nullptr) {
    return error_result<LogicalPlanPtr>(ErrorCode::PlanError,
                                        "filter requires a predicate");
  }
  if (predicate->type != SqlType::Boolean) {
    return error_result<LogicalPlanPtr>(ErrorCode::TypeMismatch,
                                        "filter predicate must be BOOLEAN");
  }
  if (child == nullptr) {
    return error_result<LogicalPlanPtr>(ErrorCode::PlanError,
                                        "filter requires an input plan");
  }

  auto filter = std::make_unique<LogicalFilter>();
  filter->predicate = std::move(predicate);
  filter->children.push_back(std::move(child));
  return ok_result<LogicalPlanPtr>(std::move(filter));
}

[[nodiscard]] Result<LogicalPlanPtr>
plan_select(const BoundSelectStatement &statement) {
  if (statement.projections.empty()) {
    return error_result<LogicalPlanPtr>(ErrorCode::PlanError,
                                        "SELECT requires at least one projection");
  }
  if (!statement.projection_names.empty() &&
      statement.projection_names.size() != statement.projections.size()) {
    return error_result<LogicalPlanPtr>(
        ErrorCode::PlanError, "projection name count does not match expressions");
  }

  LogicalPlanPtr source;
  if (!statement.table.name.empty()) {
    source = make_seq_scan_plan(statement.table);
  } else {
    const auto projection_status = ValidateNoColumnReferences(statement.projections);
    if (!status_ok(projection_status)) {
      return error_result<LogicalPlanPtr>(projection_status);
    }
    if (statement.where != nullptr) {
      const auto where_status = ValidateNoColumnReferences(*statement.where);
      if (!status_ok(where_status)) {
        return error_result<LogicalPlanPtr>(where_status);
      }
    }

    auto values = std::make_unique<LogicalValues>();
    // Scalar SELECT operates over exactly one empty input row.
    values->rows.emplace_back();
    source = std::move(values);
  }

  if (statement.where != nullptr) {
    auto predicate = CloneBoundExpression(*statement.where);
    if (!status_ok(predicate.status)) {
      return error_result<LogicalPlanPtr>(std::move(predicate.status));
    }

    auto filter = make_filter_plan(std::move(*predicate.value), std::move(source));
    if (!status_ok(filter.status)) {
      return filter;
    }
    source = std::move(*filter.value);
  }

  auto projections = CloneBoundExpressions(statement.projections);
  if (!status_ok(projections.status)) {
    return error_result<LogicalPlanPtr>(std::move(projections.status));
  }

  auto projection = std::make_unique<LogicalProjection>();
  projection->projections = std::move(*projections.value);
  projection->projection_names = statement.projection_names;
  projection->children.push_back(std::move(source));
  return ok_result<LogicalPlanPtr>(std::move(projection));
}

[[nodiscard]] Result<LogicalPlanPtr>
plan_insert(const BoundInsertStatement &statement) {
  if (statement.table.name.empty()) {
    return error_result<LogicalPlanPtr>(ErrorCode::PlanError,
                                        "INSERT requires a target table");
  }
  if (statement.values.size() != statement.table.schema.columns.size()) {
    return error_result<LogicalPlanPtr>(
        ErrorCode::PlanError, "INSERT value count does not match table schema");
  }
  const auto value_status = ValidateNoColumnReferences(statement.values);
  if (!status_ok(value_status)) {
    return error_result<LogicalPlanPtr>(value_status);
  }

  auto row = CloneBoundExpressions(statement.values);
  if (!status_ok(row.status)) {
    return error_result<LogicalPlanPtr>(std::move(row.status));
  }

  auto values = std::make_unique<LogicalValues>();
  // Keep logical VALUES as bound expressions until the storage tuple encoder is
  // available; otherwise INSERT planning would discard the row payload.
  values->rows.push_back(std::move(*row.value));

  auto insert = std::make_unique<LogicalInsert>();
  insert->table = statement.table;
  insert->children.push_back(std::move(values));
  return ok_result<LogicalPlanPtr>(std::move(insert));
}

[[nodiscard]] Result<LogicalPlanPtr>
plan_create_table(const BoundCreateTableStatement &statement) {
  if (statement.request.name.empty()) {
    return error_result<LogicalPlanPtr>(ErrorCode::PlanError,
                                        "CREATE TABLE requires a table name");
  }
  if (statement.request.schema.columns.empty()) {
    return error_result<LogicalPlanPtr>(ErrorCode::PlanError,
                                        "CREATE TABLE requires at least one column");
  }

  auto create = std::make_unique<LogicalCreateTable>();
  create->request = statement.request;
  return ok_result<LogicalPlanPtr>(std::move(create));
}

} // namespace

Result<LogicalPlanPtr> DefaultLogicalPlanner::Plan(const BoundStatement &statement) {
  if (const auto *select = dynamic_cast<const BoundSelectStatement *>(&statement)) {
    return plan_select(*select);
  }
  if (const auto *insert = dynamic_cast<const BoundInsertStatement *>(&statement)) {
    return plan_insert(*insert);
  }
  if (const auto *create =
          dynamic_cast<const BoundCreateTableStatement *>(&statement)) {
    return plan_create_table(*create);
  }

  return error_result<LogicalPlanPtr>(ErrorCode::PlanError,
                                      "unsupported bound statement");
}

} // namespace mattsql
