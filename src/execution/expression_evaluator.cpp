#include "mattsql/execution/expressions/evaluator.hpp"

#include "mattsql/common/result_utils.hpp"
#include "mattsql/common/trace.hpp"
#include "mattsql/common/value_utils.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace mattsql {
namespace {

[[nodiscard]] Result<Value> evaluate_unary(ExpressionEvaluator &evaluator,
                                           const BoundUnaryExpression &expression,
                                           const EvaluationContext &context) {
  if (expression.operand == nullptr) {
    return error_result<Value>(ErrorCode::ExecutionError,
                               "unary expression is missing its operand");
  }

  auto operand = evaluator.Evaluate(*expression.operand, context);
  if (!operand.ok()) {
    return operand;
  }

  switch (expression.op) {
  case UnaryOperator::Plus: {
    auto integer = RequireInteger(operand.Value(), "unary plus");
    if (!integer.ok()) {
      return error_result<Value>(integer.status);
    }
    return ok_result<Value>(integer.Value());
  }
  case UnaryOperator::Minus: {
    auto integer = RequireInteger(operand.Value(), "unary minus");
    if (!integer.ok()) {
      return error_result<Value>(integer.status);
    }
    auto negated = CheckedIntegerNegate(integer.Value(), "unary minus");
    if (!negated.ok()) {
      return error_result<Value>(std::move(negated.status));
    }
    return ok_result<Value>(negated.Value());
  }
  case UnaryOperator::Not: {
    auto boolean = RequireBoolean(operand.Value(), "NOT");
    if (!boolean.ok()) {
      return error_result<Value>(boolean.status);
    }
    return ok_result<Value>(!boolean.Value());
  }
  }

  return error_result<Value>(ErrorCode::ExecutionError, "unknown unary operator");
}

[[nodiscard]] Result<Value>
evaluate_logical_and(ExpressionEvaluator &evaluator,
                     const BoundBinaryExpression &expression,
                     const EvaluationContext &context) {
  auto left = evaluator.Evaluate(*expression.left, context);
  if (!left.ok()) {
    return left;
  }
  auto left_boolean = RequireBoolean(left.Value(), "AND");
  if (!left_boolean.ok()) {
    return error_result<Value>(left_boolean.status);
  }
  if (!left_boolean.Value()) {
    return ok_result<Value>(false);
  }

  auto right = evaluator.Evaluate(*expression.right, context);
  if (!right.ok()) {
    return right;
  }
  auto right_boolean = RequireBoolean(right.Value(), "AND");
  if (!right_boolean.ok()) {
    return error_result<Value>(right_boolean.status);
  }
  return ok_result<Value>(right_boolean.Value());
}

[[nodiscard]] Result<Value> evaluate_logical_or(ExpressionEvaluator &evaluator,
                                                const BoundBinaryExpression &expression,
                                                const EvaluationContext &context) {
  auto left = evaluator.Evaluate(*expression.left, context);
  if (!left.ok()) {
    return left;
  }
  auto left_boolean = RequireBoolean(left.Value(), "OR");
  if (!left_boolean.ok()) {
    return error_result<Value>(left_boolean.status);
  }
  if (left_boolean.Value()) {
    return ok_result<Value>(true);
  }

  auto right = evaluator.Evaluate(*expression.right, context);
  if (!right.ok()) {
    return right;
  }
  auto right_boolean = RequireBoolean(right.Value(), "OR");
  if (!right_boolean.ok()) {
    return error_result<Value>(right_boolean.status);
  }
  return ok_result<Value>(right_boolean.Value());
}

[[nodiscard]] Result<Value> evaluate_binary(ExpressionEvaluator &evaluator,
                                            const BoundBinaryExpression &expression,
                                            const EvaluationContext &context) {
  if (expression.left == nullptr || expression.right == nullptr) {
    return error_result<Value>(ErrorCode::ExecutionError,
                               "binary expression is missing an operand");
  }

  if (expression.op == BinaryOperator::And) {
    return evaluate_logical_and(evaluator, expression, context);
  }
  if (expression.op == BinaryOperator::Or) {
    return evaluate_logical_or(evaluator, expression, context);
  }

  auto left = evaluator.Evaluate(*expression.left, context);
  if (!left.ok()) {
    return left;
  }

  auto right = evaluator.Evaluate(*expression.right, context);
  if (!right.ok()) {
    return right;
  }

  switch (expression.op) {
  case BinaryOperator::Add:
  case BinaryOperator::Subtract:
  case BinaryOperator::Multiply:
  case BinaryOperator::Divide: {
    auto left_integer = RequireInteger(left.Value(), "arithmetic");
    auto right_integer = RequireInteger(right.Value(), "arithmetic");
    if (!left_integer.ok()) {
      return error_result<Value>(left_integer.status);
    }
    if (!right_integer.ok()) {
      return error_result<Value>(right_integer.status);
    }

    switch (expression.op) {
    case BinaryOperator::Add: {
      auto result = CheckedIntegerAdd(left_integer.Value(), right_integer.Value(),
                                      "integer addition");
      if (!result.ok()) {
        return error_result<Value>(std::move(result.status));
      }
      return ok_result<Value>(result.Value());
    }
    case BinaryOperator::Subtract: {
      auto result = CheckedIntegerSubtract(left_integer.Value(), right_integer.Value(),
                                           "integer subtraction");
      if (!result.ok()) {
        return error_result<Value>(std::move(result.status));
      }
      return ok_result<Value>(result.Value());
    }
    case BinaryOperator::Multiply: {
      auto result = CheckedIntegerMultiply(left_integer.Value(), right_integer.Value(),
                                           "integer multiplication");
      if (!result.ok()) {
        return error_result<Value>(std::move(result.status));
      }
      return ok_result<Value>(result.Value());
    }
    case BinaryOperator::Divide: {
      auto result = CheckedIntegerDivide(left_integer.Value(), right_integer.Value(),
                                         "integer division");
      if (!result.ok()) {
        return error_result<Value>(std::move(result.status));
      }
      return ok_result<Value>(result.Value());
    }
    default:
      break;
    }
    break;
  }

  case BinaryOperator::Equal:
    if (IsNull(left.Value()) || IsNull(right.Value())) {
      return ok_result<Value>(false);
    }
    if (auto comparison = CompareValues(left.Value(), right.Value()); comparison.ok()) {
      return ok_result<Value>(comparison.Value() == 0);
    } else {
      return error_result<Value>(comparison.status);
    }

  case BinaryOperator::NotEqual:
    if (IsNull(left.Value()) || IsNull(right.Value())) {
      return ok_result<Value>(false);
    }
    if (auto comparison = CompareValues(left.Value(), right.Value()); comparison.ok()) {
      return ok_result<Value>(comparison.Value() != 0);
    } else {
      return error_result<Value>(comparison.status);
    }

  case BinaryOperator::Less:
  case BinaryOperator::LessEqual:
  case BinaryOperator::Greater:
  case BinaryOperator::GreaterEqual: {
    auto comparison = CompareValues(left.Value(), right.Value());
    if (!comparison.ok()) {
      return error_result<Value>(comparison.status);
    }

    switch (expression.op) {
    case BinaryOperator::Less:
      return ok_result<Value>(comparison.Value() < 0);
    case BinaryOperator::LessEqual:
      return ok_result<Value>(comparison.Value() <= 0);
    case BinaryOperator::Greater:
      return ok_result<Value>(comparison.Value() > 0);
    case BinaryOperator::GreaterEqual:
      return ok_result<Value>(comparison.Value() >= 0);
    default:
      break;
    }
    break;
  }
  case BinaryOperator::And:
  case BinaryOperator::Or:
    break;
  }

  return error_result<Value>(ErrorCode::ExecutionError, "unknown binary operator");
}

} // namespace

Result<Value> DefaultExpressionEvaluator::Evaluate(const BoundExpression &expression,
                                                   const EvaluationContext &context) {
  ScopedTrace trace("mattsql::DefaultExpressionEvaluator::Evaluate",
                    "function.execution");
  if (const auto *literal =
          dynamic_cast<const BoundLiteralExpression *>(&expression)) {
    return ok_result(literal->value);
  }

  if (const auto *column = dynamic_cast<const BoundColumnExpression *>(&expression)) {
    if (context.schema == nullptr || context.row == nullptr) {
      return error_result<Value>(ErrorCode::ExecutionError,
                                 "column expression requires a row context");
    }
    if (column->column_id >= context.row->size() ||
        column->column_id >= context.schema->columns.size()) {
      return error_result<Value>(ErrorCode::ExecutionError,
                                 "column id is out of range");
    }
    return ok_result((*context.row)[column->column_id]);
  }

  if (const auto *unary = dynamic_cast<const BoundUnaryExpression *>(&expression)) {
    return evaluate_unary(*this, *unary, context);
  }

  if (const auto *binary = dynamic_cast<const BoundBinaryExpression *>(&expression)) {
    return evaluate_binary(*this, *binary, context);
  }

  return error_result<Value>(ErrorCode::ExecutionError, "unsupported expression");
}

std::string ProjectionName(const BoundExpression &expression, std::size_t index) {
  if (const auto *column = dynamic_cast<const BoundColumnExpression *>(&expression)) {
    return column->column_name;
  }

  return "expr" + std::to_string(index + 1);
}

Result<std::vector<Value>>
EvaluateExpressions(ExpressionEvaluator &evaluator,
                    const std::vector<BoundExpressionPtr> &expressions,
                    const EvaluationContext &context) {
  ScopedTrace trace("mattsql::EvaluateExpressions", "function.execution");
  std::vector<Value> values;
  values.reserve(expressions.size());

  for (const auto &expression : expressions) {
    if (expression == nullptr) {
      return error_result<std::vector<Value>>(ErrorCode::ExecutionError,
                                              "expression cannot be null");
    }

    auto value = evaluator.Evaluate(*expression, context);
    if (!value.ok()) {
      return error_result<std::vector<Value>>(std::move(value.status));
    }
    values.push_back(std::move(value).TakeValue());
  }

  return ok_result(std::move(values));
}

} // namespace mattsql
