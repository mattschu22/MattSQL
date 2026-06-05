#include "mattsql/optimizer/default_optimizer.hpp"

#include "mattsql/binder/expression_utils.hpp"
#include "mattsql/common/result_utils.hpp"
#include "mattsql/common/value_utils.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace mattsql {
namespace {

[[nodiscard]] std::optional<bool> boolean_literal(const BoundExpression &expression) {
  const auto *value = BoundLiteralValue(expression);
  if (value == nullptr) {
    return std::nullopt;
  }

  if (const auto *boolean = std::get_if<bool>(value)) {
    return *boolean;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<Value> fold_unary(UnaryOperator op, const Value &operand) {
  switch (op) {
  case UnaryOperator::Plus:
    if (const auto *integer = std::get_if<std::int64_t>(&operand)) {
      return *integer;
    }
    return std::nullopt;
  case UnaryOperator::Minus:
    if (const auto *integer = std::get_if<std::int64_t>(&operand)) {
      return -*integer;
    }
    return std::nullopt;
  case UnaryOperator::Not:
    if (const auto *boolean = std::get_if<bool>(&operand)) {
      return !*boolean;
    }
    return std::nullopt;
  }

  return std::nullopt;
}

[[nodiscard]] std::optional<Value> fold_arithmetic(BinaryOperator op, const Value &left,
                                                   const Value &right) {
  const auto *left_integer = std::get_if<std::int64_t>(&left);
  const auto *right_integer = std::get_if<std::int64_t>(&right);
  if (left_integer == nullptr || right_integer == nullptr) {
    return std::nullopt;
  }

  switch (op) {
  case BinaryOperator::Add:
    return *left_integer + *right_integer;
  case BinaryOperator::Subtract:
    return *left_integer - *right_integer;
  case BinaryOperator::Multiply:
    return *left_integer * *right_integer;
  case BinaryOperator::Divide:
    if (*right_integer == 0) {
      return std::nullopt;
    }
    return *left_integer / *right_integer;
  default:
    return std::nullopt;
  }
}

[[nodiscard]] std::optional<Value> fold_comparison(BinaryOperator op, const Value &left,
                                                   const Value &right) {
  switch (op) {
  case BinaryOperator::Equal:
    if (IsNull(left) || IsNull(right)) {
      return false;
    }
    if (auto comparison = CompareValues(left, right); status_ok(comparison.status)) {
      return *comparison.value == 0;
    }
    return std::nullopt;
  case BinaryOperator::NotEqual:
    if (IsNull(left) || IsNull(right)) {
      return false;
    }
    if (auto comparison = CompareValues(left, right); status_ok(comparison.status)) {
      return *comparison.value != 0;
    }
    return std::nullopt;
  case BinaryOperator::Less:
    if (auto comparison = CompareValues(left, right); status_ok(comparison.status)) {
      return *comparison.value < 0;
    }
    return std::nullopt;
  case BinaryOperator::LessEqual:
    if (auto comparison = CompareValues(left, right); status_ok(comparison.status)) {
      return *comparison.value <= 0;
    }
    return std::nullopt;
  case BinaryOperator::Greater:
    if (auto comparison = CompareValues(left, right); status_ok(comparison.status)) {
      return *comparison.value > 0;
    }
    return std::nullopt;
  case BinaryOperator::GreaterEqual:
    if (auto comparison = CompareValues(left, right); status_ok(comparison.status)) {
      return *comparison.value >= 0;
    }
    return std::nullopt;
  default:
    return std::nullopt;
  }
}

[[nodiscard]] std::optional<Value> fold_boolean(BinaryOperator op, const Value &left,
                                                const Value &right) {
  const auto *left_boolean = std::get_if<bool>(&left);
  const auto *right_boolean = std::get_if<bool>(&right);
  if (left_boolean == nullptr || right_boolean == nullptr) {
    return std::nullopt;
  }

  switch (op) {
  case BinaryOperator::And:
    return *left_boolean && *right_boolean;
  case BinaryOperator::Or:
    return *left_boolean || *right_boolean;
  default:
    return std::nullopt;
  }
}

[[nodiscard]] std::optional<Value> fold_binary(BinaryOperator op, const Value &left,
                                               const Value &right) {
  if (const auto value = fold_arithmetic(op, left, right)) {
    return value;
  }
  if (const auto value = fold_comparison(op, left, right)) {
    return value;
  }
  return fold_boolean(op, left, right);
}

[[nodiscard]] Result<BoundExpressionPtr>
fold_current_expression(BoundExpressionPtr expression) {
  if (auto *unary = dynamic_cast<BoundUnaryExpression *>(expression.get())) {
    if (unary->operand == nullptr) {
      return error_result<BoundExpressionPtr>(
          ErrorCode::PlanError, "unary expression is missing its operand");
    }

    if (const auto *operand_value = BoundLiteralValue(*unary->operand)) {
      if (const auto folded = fold_unary(unary->op, *operand_value)) {
        return ok_result(MakeBoundLiteral(*folded));
      }
    }

    return ok_result(std::move(expression));
  }

  if (auto *binary = dynamic_cast<BoundBinaryExpression *>(expression.get())) {
    if (binary->left == nullptr || binary->right == nullptr) {
      return error_result<BoundExpressionPtr>(
          ErrorCode::PlanError, "binary expression is missing an operand");
    }

    const auto *left_value = BoundLiteralValue(*binary->left);
    const auto *right_value = BoundLiteralValue(*binary->right);
    if (left_value != nullptr && right_value != nullptr) {
      if (const auto folded = fold_binary(binary->op, *left_value, *right_value)) {
        return ok_result(MakeBoundLiteral(*folded));
      }
    }

    return ok_result(std::move(expression));
  }

  return ok_result(std::move(expression));
}

[[nodiscard]] Result<BoundExpressionPtr>
fold_expression(BoundExpressionPtr expression) {
  return TransformBoundExpression(std::move(expression), fold_current_expression);
}

[[nodiscard]] Result<LogicalPlanPtr> fold_plan(LogicalPlanPtr plan) {
  if (plan == nullptr) {
    return error_result<LogicalPlanPtr>(ErrorCode::InvalidArgument,
                                        "logical plan is required");
  }

  for (auto &child : plan->children) {
    auto folded = fold_plan(std::move(child));
    if (!status_ok(folded.status)) {
      return folded;
    }
    child = std::move(*folded.value);
  }

  if (auto *filter = dynamic_cast<LogicalFilter *>(plan.get())) {
    auto predicate = fold_expression(std::move(filter->predicate));
    if (!status_ok(predicate.status)) {
      return error_result<LogicalPlanPtr>(std::move(predicate.status));
    }
    filter->predicate = std::move(*predicate.value);
    return ok_result(std::move(plan));
  }

  if (auto *projection = dynamic_cast<LogicalProjection *>(plan.get())) {
    for (auto &projection_expression : projection->projections) {
      auto folded = fold_expression(std::move(projection_expression));
      if (!status_ok(folded.status)) {
        return error_result<LogicalPlanPtr>(std::move(folded.status));
      }
      projection_expression = std::move(*folded.value);
    }
    return ok_result(std::move(plan));
  }

  if (auto *values = dynamic_cast<LogicalValues *>(plan.get())) {
    for (auto &row : values->rows) {
      for (auto &value_expression : row) {
        auto folded = fold_expression(std::move(value_expression));
        if (!status_ok(folded.status)) {
          return error_result<LogicalPlanPtr>(std::move(folded.status));
        }
        value_expression = std::move(*folded.value);
      }
    }
    return ok_result(std::move(plan));
  }

  return ok_result(std::move(plan));
}

[[nodiscard]] Result<LogicalPlanPtr> remove_constant_filters(LogicalPlanPtr plan) {
  if (plan == nullptr) {
    return error_result<LogicalPlanPtr>(ErrorCode::InvalidArgument,
                                        "logical plan is required");
  }

  for (auto &child : plan->children) {
    auto optimized = remove_constant_filters(std::move(child));
    if (!status_ok(optimized.status)) {
      return optimized;
    }
    child = std::move(*optimized.value);
  }

  auto *filter = dynamic_cast<LogicalFilter *>(plan.get());
  if (filter == nullptr) {
    return ok_result(std::move(plan));
  }

  if (filter->predicate == nullptr) {
    return error_result<LogicalPlanPtr>(ErrorCode::PlanError,
                                        "filter requires a predicate");
  }

  const auto predicate = boolean_literal(*filter->predicate);
  if (!predicate.has_value()) {
    return ok_result(std::move(plan));
  }

  if (*predicate) {
    if (filter->children.size() != 1 || filter->children[0] == nullptr) {
      return error_result<LogicalPlanPtr>(ErrorCode::PlanError,
                                          "filter requires one input plan");
    }
    return ok_result(std::move(filter->children[0]));
  }

  auto values = std::make_unique<LogicalValues>();
  values->kind = LogicalOperatorKind::Values;
  return ok_result<LogicalPlanPtr>(std::move(values));
}

[[nodiscard]] bool
has_rule_named(const std::vector<std::unique_ptr<OptimizerRule>> &rules,
               std::string_view name) {
  for (const auto &rule : rules) {
    if (rule != nullptr && rule->Name() == name) {
      return true;
    }
  }
  return false;
}

} // namespace

std::string_view ConstantFoldingRule::Name() const { return "constant_folding"; }

Result<LogicalPlanPtr> ConstantFoldingRule::Apply(LogicalPlanPtr plan) {
  return fold_plan(std::move(plan));
}

std::string_view RemoveConstantFilterRule::Name() const {
  return "remove_constant_filter";
}

Result<LogicalPlanPtr> RemoveConstantFilterRule::Apply(LogicalPlanPtr plan) {
  return remove_constant_filters(std::move(plan));
}

DefaultOptimizer::DefaultOptimizer() {
  rules_.push_back(std::make_unique<ConstantFoldingRule>());
  rules_.push_back(std::make_unique<RemoveConstantFilterRule>());
}

Status DefaultOptimizer::AddRule(std::unique_ptr<OptimizerRule> rule) {
  if (rule == nullptr) {
    return error_status(ErrorCode::InvalidArgument, "optimizer rule is required");
  }
  if (rule->Name().empty()) {
    return error_status(ErrorCode::InvalidArgument, "optimizer rule name is required");
  }
  if (has_rule_named(rules_, rule->Name())) {
    return error_status(ErrorCode::AlreadyExists, "optimizer rule already exists");
  }

  rules_.push_back(std::move(rule));
  return ok_status();
}

Result<LogicalPlanPtr> DefaultOptimizer::Optimize(LogicalPlanPtr plan) {
  if (plan == nullptr) {
    return error_result<LogicalPlanPtr>(ErrorCode::InvalidArgument,
                                        "logical plan is required");
  }

  for (const auto &rule : rules_) {
    auto optimized = rule->Apply(std::move(plan));
    if (!status_ok(optimized.status)) {
      return optimized;
    }
    if (!optimized.value.has_value() || *optimized.value == nullptr) {
      return error_result<LogicalPlanPtr>(
          ErrorCode::Internal,
          "optimizer rule returned success without a logical plan: " +
              std::string(rule->Name()));
    }
    plan = std::move(*optimized.value);
  }

  return ok_result(std::move(plan));
}

} // namespace mattsql
