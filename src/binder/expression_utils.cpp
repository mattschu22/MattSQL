#include "mattsql/binder/expression_utils.hpp"

#include "mattsql/common/result_utils.hpp"
#include "mattsql/common/value_utils.hpp"

#include <memory>
#include <utility>

namespace mattsql {

BoundExpressionPtr MakeBoundLiteral(Value value) {
  const auto type = ValueTypeOf(value);
  return MakeBoundLiteral(std::move(value), type);
}

BoundExpressionPtr MakeBoundLiteral(Value value, SqlType type) {
  auto bound = std::make_unique<BoundLiteralExpression>();
  bound->type = type;
  bound->value = std::move(value);
  return bound;
}

BoundExpressionPtr MakeBoundColumn(const TableInfo &table, const ColumnSchema &column) {
  auto bound = std::make_unique<BoundColumnExpression>();
  bound->type = column.type;
  bound->table_id = table.id;
  bound->column_id = column.id;
  bound->table_name = table.name;
  bound->column_name = column.name;
  return bound;
}

const Value *BoundLiteralValue(const BoundExpression &expression) {
  const auto *literal = dynamic_cast<const BoundLiteralExpression *>(&expression);
  return literal == nullptr ? nullptr : &literal->value;
}

Result<BoundExpressionPtr> CloneBoundExpression(const BoundExpression &expression) {
  if (const auto *literal =
          dynamic_cast<const BoundLiteralExpression *>(&expression)) {
    return ok_result(MakeBoundLiteral(literal->value, literal->type));
  }

  if (const auto *column =
          dynamic_cast<const BoundColumnExpression *>(&expression)) {
    auto clone = std::make_unique<BoundColumnExpression>();
    clone->type = column->type;
    clone->table_id = column->table_id;
    clone->column_id = column->column_id;
    clone->table_name = column->table_name;
    clone->column_name = column->column_name;
    return ok_result<BoundExpressionPtr>(std::move(clone));
  }

  if (const auto *unary = dynamic_cast<const BoundUnaryExpression *>(&expression)) {
    if (unary->operand == nullptr) {
      return error_result<BoundExpressionPtr>(
          ErrorCode::PlanError, "unary expression is missing its operand");
    }

    auto operand = CloneBoundExpression(*unary->operand);
    if (!status_ok(operand.status)) {
      return operand;
    }

    auto clone = std::make_unique<BoundUnaryExpression>();
    clone->type = unary->type;
    clone->op = unary->op;
    clone->operand = std::move(*operand.value);
    return ok_result<BoundExpressionPtr>(std::move(clone));
  }

  if (const auto *binary =
          dynamic_cast<const BoundBinaryExpression *>(&expression)) {
    if (binary->left == nullptr || binary->right == nullptr) {
      return error_result<BoundExpressionPtr>(
          ErrorCode::PlanError, "binary expression is missing an operand");
    }

    auto left = CloneBoundExpression(*binary->left);
    if (!status_ok(left.status)) {
      return left;
    }

    auto right = CloneBoundExpression(*binary->right);
    if (!status_ok(right.status)) {
      return right;
    }

    auto clone = std::make_unique<BoundBinaryExpression>();
    clone->type = binary->type;
    clone->left = std::move(*left.value);
    clone->op = binary->op;
    clone->right = std::move(*right.value);
    return ok_result<BoundExpressionPtr>(std::move(clone));
  }

  if (dynamic_cast<const BoundStarExpression *>(&expression) != nullptr) {
    return error_result<BoundExpressionPtr>(
        ErrorCode::PlanError, "star expressions must be expanded before planning");
  }

  return error_result<BoundExpressionPtr>(ErrorCode::PlanError,
                                          "unsupported bound expression");
}

Result<std::vector<BoundExpressionPtr>>
CloneBoundExpressions(const std::vector<BoundExpressionPtr> &expressions) {
  std::vector<BoundExpressionPtr> clones;
  clones.reserve(expressions.size());

  for (const auto &expression : expressions) {
    if (expression == nullptr) {
      return error_result<std::vector<BoundExpressionPtr>>(
          ErrorCode::PlanError, "expression list contains a null expression");
    }

    auto clone = CloneBoundExpression(*expression);
    if (!status_ok(clone.status)) {
      return error_result<std::vector<BoundExpressionPtr>>(std::move(clone.status));
    }
    clones.push_back(std::move(*clone.value));
  }

  return ok_result(std::move(clones));
}

Result<std::vector<std::vector<BoundExpressionPtr>>>
CloneBoundExpressionRows(const std::vector<std::vector<BoundExpressionPtr>> &rows) {
  std::vector<std::vector<BoundExpressionPtr>> clones;
  clones.reserve(rows.size());

  for (const auto &row : rows) {
    auto clone = CloneBoundExpressions(row);
    if (!status_ok(clone.status)) {
      return error_result<std::vector<std::vector<BoundExpressionPtr>>>(
          std::move(clone.status));
    }
    clones.push_back(std::move(*clone.value));
  }

  return ok_result(std::move(clones));
}

Status WalkBoundExpression(const BoundExpression &expression,
                           const BoundExpressionVisitor &visitor) {
  const auto visit_status = visitor(expression);
  if (!status_ok(visit_status)) {
    return visit_status;
  }

  if (dynamic_cast<const BoundLiteralExpression *>(&expression) != nullptr ||
      dynamic_cast<const BoundColumnExpression *>(&expression) != nullptr ||
      dynamic_cast<const BoundStarExpression *>(&expression) != nullptr) {
    return ok_status();
  }

  if (const auto *unary = dynamic_cast<const BoundUnaryExpression *>(&expression)) {
    if (unary->operand == nullptr) {
      return error_status(ErrorCode::PlanError,
                          "unary expression is missing its operand");
    }
    return WalkBoundExpression(*unary->operand, visitor);
  }

  if (const auto *binary =
          dynamic_cast<const BoundBinaryExpression *>(&expression)) {
    if (binary->left == nullptr || binary->right == nullptr) {
      return error_status(ErrorCode::PlanError,
                          "binary expression is missing an operand");
    }

    const auto left_status = WalkBoundExpression(*binary->left, visitor);
    if (!status_ok(left_status)) {
      return left_status;
    }
    return WalkBoundExpression(*binary->right, visitor);
  }

  return error_status(ErrorCode::PlanError, "unsupported bound expression");
}

Result<BoundExpressionPtr>
TransformBoundExpression(BoundExpressionPtr expression,
                         const BoundExpressionTransform &transform) {
  if (expression == nullptr) {
    return error_result<BoundExpressionPtr>(ErrorCode::PlanError,
                                            "expression cannot be null");
  }
  if (dynamic_cast<BoundStarExpression *>(expression.get()) != nullptr) {
    return error_result<BoundExpressionPtr>(
        ErrorCode::PlanError,
        "star expressions must be expanded before expression transforms");
  }

  if (auto *unary = dynamic_cast<BoundUnaryExpression *>(expression.get())) {
    auto operand = TransformBoundExpression(std::move(unary->operand), transform);
    if (!status_ok(operand.status)) {
      return operand;
    }
    unary->operand = std::move(*operand.value);
    return transform(std::move(expression));
  }

  if (auto *binary = dynamic_cast<BoundBinaryExpression *>(expression.get())) {
    auto left = TransformBoundExpression(std::move(binary->left), transform);
    if (!status_ok(left.status)) {
      return left;
    }
    binary->left = std::move(*left.value);

    auto right = TransformBoundExpression(std::move(binary->right), transform);
    if (!status_ok(right.status)) {
      return right;
    }
    binary->right = std::move(*right.value);
    return transform(std::move(expression));
  }

  if (dynamic_cast<BoundLiteralExpression *>(expression.get()) != nullptr ||
      dynamic_cast<BoundColumnExpression *>(expression.get()) != nullptr) {
    return transform(std::move(expression));
  }

  return error_result<BoundExpressionPtr>(ErrorCode::PlanError,
                                          "unsupported bound expression");
}

Status ValidateNoColumnReferences(const BoundExpression &expression) {
  return WalkBoundExpression(expression, [](const BoundExpression &current) -> Status {
    if (dynamic_cast<const BoundColumnExpression *>(&current) != nullptr) {
      return error_status(ErrorCode::PlanError,
                          "column expression requires a table-producing input plan");
    }
    if (dynamic_cast<const BoundStarExpression *>(&current) != nullptr) {
      return error_status(ErrorCode::PlanError,
                          "star expressions must be expanded before planning");
    }

    return ok_status();
  });
}

Status ValidateNoColumnReferences(const std::vector<BoundExpressionPtr> &expressions) {
  for (const auto &expression : expressions) {
    if (expression == nullptr) {
      return error_status(ErrorCode::PlanError,
                          "expression list contains a null expression");
    }

    const auto status = ValidateNoColumnReferences(*expression);
    if (!status_ok(status)) {
      return status;
    }
  }

  return ok_status();
}

} // namespace mattsql
