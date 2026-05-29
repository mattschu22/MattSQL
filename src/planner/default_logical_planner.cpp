#include "mattsql/planner/default_logical_planner.hpp"

#include "mattsql/common/result_utils.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mattsql {
namespace {

[[nodiscard]] Result<BoundExpressionPtr>
clone_expression(const BoundExpression &expression);

[[nodiscard]] Result<BoundExpressionPtr>
clone_literal(const BoundLiteralExpression &expression) {
  auto clone = std::make_unique<BoundLiteralExpression>();
  clone->kind = BoundExpressionKind::Literal;
  clone->type = expression.type;
  clone->value = expression.value;
  return ok_result<BoundExpressionPtr>(std::move(clone));
}

[[nodiscard]] Result<BoundExpressionPtr>
clone_column(const BoundColumnExpression &expression) {
  auto clone = std::make_unique<BoundColumnExpression>();
  clone->kind = BoundExpressionKind::Column;
  clone->type = expression.type;
  clone->table_id = expression.table_id;
  clone->column_id = expression.column_id;
  clone->table_name = expression.table_name;
  clone->column_name = expression.column_name;
  return ok_result<BoundExpressionPtr>(std::move(clone));
}

[[nodiscard]] Result<BoundExpressionPtr>
clone_unary(const BoundUnaryExpression &expression) {
  if (expression.operand == nullptr) {
    return error_result<BoundExpressionPtr>(ErrorCode::PlanError,
                                            "unary expression is missing its operand");
  }

  auto operand = clone_expression(*expression.operand);
  if (!status_ok(operand.status)) {
    return operand;
  }

  auto clone = std::make_unique<BoundUnaryExpression>();
  clone->kind = BoundExpressionKind::Unary;
  clone->type = expression.type;
  clone->op = expression.op;
  clone->operand = std::move(*operand.value);
  return ok_result<BoundExpressionPtr>(std::move(clone));
}

[[nodiscard]] Result<BoundExpressionPtr>
clone_binary(const BoundBinaryExpression &expression) {
  if (expression.left == nullptr || expression.right == nullptr) {
    return error_result<BoundExpressionPtr>(ErrorCode::PlanError,
                                            "binary expression is missing an operand");
  }

  auto left = clone_expression(*expression.left);
  if (!status_ok(left.status)) {
    return left;
  }

  auto right = clone_expression(*expression.right);
  if (!status_ok(right.status)) {
    return right;
  }

  auto clone = std::make_unique<BoundBinaryExpression>();
  clone->kind = BoundExpressionKind::Binary;
  clone->type = expression.type;
  clone->left = std::move(*left.value);
  clone->op = expression.op;
  clone->right = std::move(*right.value);
  return ok_result<BoundExpressionPtr>(std::move(clone));
}

[[nodiscard]] Result<BoundExpressionPtr>
clone_star(const BoundStarExpression &expression) {
  (void)expression;
  return error_result<BoundExpressionPtr>(
      ErrorCode::PlanError, "star expressions must be expanded before planning");
}

[[nodiscard]] Result<BoundExpressionPtr>
clone_expression(const BoundExpression &expression) {
  if (const auto *literal = dynamic_cast<const BoundLiteralExpression *>(&expression)) {
    return clone_literal(*literal);
  }
  if (const auto *column = dynamic_cast<const BoundColumnExpression *>(&expression)) {
    return clone_column(*column);
  }
  if (const auto *unary = dynamic_cast<const BoundUnaryExpression *>(&expression)) {
    return clone_unary(*unary);
  }
  if (const auto *binary = dynamic_cast<const BoundBinaryExpression *>(&expression)) {
    return clone_binary(*binary);
  }
  if (const auto *star = dynamic_cast<const BoundStarExpression *>(&expression)) {
    return clone_star(*star);
  }

  return error_result<BoundExpressionPtr>(ErrorCode::PlanError,
                                          "unsupported bound expression");
}

[[nodiscard]] Status validate_no_column_references(const BoundExpression &expression) {
  if (dynamic_cast<const BoundLiteralExpression *>(&expression) != nullptr) {
    return {};
  }
  if (dynamic_cast<const BoundColumnExpression *>(&expression) != nullptr) {
    return error_status(ErrorCode::PlanError,
                        "column expression requires a table-producing input plan");
  }
  if (dynamic_cast<const BoundStarExpression *>(&expression) != nullptr) {
    return error_status(ErrorCode::PlanError,
                        "star expressions must be expanded before planning");
  }
  if (const auto *unary = dynamic_cast<const BoundUnaryExpression *>(&expression)) {
    if (unary->operand == nullptr) {
      return error_status(ErrorCode::PlanError,
                          "unary expression is missing its operand");
    }
    return validate_no_column_references(*unary->operand);
  }
  if (const auto *binary = dynamic_cast<const BoundBinaryExpression *>(&expression)) {
    if (binary->left == nullptr || binary->right == nullptr) {
      return error_status(ErrorCode::PlanError,
                          "binary expression is missing an operand");
    }

    const auto left_status = validate_no_column_references(*binary->left);
    if (!status_ok(left_status)) {
      return left_status;
    }
    return validate_no_column_references(*binary->right);
  }

  return error_status(ErrorCode::PlanError, "unsupported bound expression");
}

[[nodiscard]] Status
validate_no_column_references(const std::vector<BoundExpressionPtr> &expressions) {
  for (const auto &expression : expressions) {
    if (expression == nullptr) {
      return error_status(ErrorCode::PlanError,
                          "expression list contains a null expression");
    }

    const auto status = validate_no_column_references(*expression);
    if (!status_ok(status)) {
      return status;
    }
  }

  return {};
}

[[nodiscard]] Result<std::vector<BoundExpressionPtr>>
clone_expressions(const std::vector<BoundExpressionPtr> &expressions) {
  std::vector<BoundExpressionPtr> clones;
  clones.reserve(expressions.size());

  for (const auto &expression : expressions) {
    if (expression == nullptr) {
      return error_result<std::vector<BoundExpressionPtr>>(
          ErrorCode::PlanError, "expression list contains a null expression");
    }

    auto clone = clone_expression(*expression);
    if (!status_ok(clone.status)) {
      return error_result<std::vector<BoundExpressionPtr>>(clone.status);
    }

    clones.push_back(std::move(*clone.value));
  }

  return ok_result(std::move(clones));
}

[[nodiscard]] LogicalPlanPtr make_seq_scan_plan(const TableInfo &table) {
  auto scan = std::make_unique<LogicalSeqScan>();
  scan->kind = LogicalOperatorKind::SeqScan;
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
  filter->kind = LogicalOperatorKind::Filter;
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

  LogicalPlanPtr source;
  if (!statement.table.name.empty()) {
    source = make_seq_scan_plan(statement.table);
  } else {
    const auto projection_status = validate_no_column_references(statement.projections);
    if (!status_ok(projection_status)) {
      return error_result<LogicalPlanPtr>(projection_status);
    }
    if (statement.where != nullptr) {
      const auto where_status = validate_no_column_references(*statement.where);
      if (!status_ok(where_status)) {
        return error_result<LogicalPlanPtr>(where_status);
      }
    }

    auto values = std::make_unique<LogicalValues>();
    values->kind = LogicalOperatorKind::Values;
    // Scalar SELECT operates over exactly one empty input row.
    values->rows.emplace_back();
    source = std::move(values);
  }

  if (statement.where != nullptr) {
    auto predicate = clone_expression(*statement.where);
    if (!status_ok(predicate.status)) {
      return error_result<LogicalPlanPtr>(std::move(predicate.status));
    }

    auto filter = make_filter_plan(std::move(*predicate.value), std::move(source));
    if (!status_ok(filter.status)) {
      return filter;
    }
    source = std::move(*filter.value);
  }

  auto projections = clone_expressions(statement.projections);
  if (!status_ok(projections.status)) {
    return error_result<LogicalPlanPtr>(std::move(projections.status));
  }

  auto projection = std::make_unique<LogicalProjection>();
  projection->kind = LogicalOperatorKind::Projection;
  projection->projections = std::move(*projections.value);
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
  const auto value_status = validate_no_column_references(statement.values);
  if (!status_ok(value_status)) {
    return error_result<LogicalPlanPtr>(value_status);
  }

  auto row = clone_expressions(statement.values);
  if (!status_ok(row.status)) {
    return error_result<LogicalPlanPtr>(std::move(row.status));
  }

  auto values = std::make_unique<LogicalValues>();
  values->kind = LogicalOperatorKind::Values;
  // Keep logical VALUES as bound expressions until the storage tuple encoder is
  // available; otherwise INSERT planning would discard the row payload.
  values->rows.push_back(std::move(*row.value));

  auto insert = std::make_unique<LogicalInsert>();
  insert->kind = LogicalOperatorKind::Insert;
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
  create->kind = LogicalOperatorKind::CreateTable;
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
