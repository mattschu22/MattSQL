#include "mattsql/optimizer/default_optimizer.hpp"

#include "mattsql/common/result_utils.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace mattsql {
namespace {

[[nodiscard]] bool is_null(const Value &value) {
  return std::holds_alternative<NullValue>(value);
}

[[nodiscard]] SqlType value_type(const Value &value) {
  if (std::holds_alternative<std::int64_t>(value)) {
    return SqlType::Integer;
  }
  if (std::holds_alternative<std::string>(value)) {
    return SqlType::Text;
  }
  if (std::holds_alternative<bool>(value)) {
    return SqlType::Boolean;
  }
  return SqlType::Null;
}

[[nodiscard]] BoundExpressionPtr make_literal(Value value) {
  auto literal = std::make_unique<BoundLiteralExpression>();
  literal->kind = BoundExpressionKind::Literal;
  literal->type = value_type(value);
  literal->value = std::move(value);
  return literal;
}

[[nodiscard]] const Value *literal_value(const BoundExpression &expression) {
  const auto *literal = dynamic_cast<const BoundLiteralExpression *>(&expression);
  if (literal == nullptr) {
    return nullptr;
  }
  return &literal->value;
}

[[nodiscard]] std::optional<bool> boolean_literal(const BoundExpression &expression) {
  const auto *value = literal_value(expression);
  if (value == nullptr) {
    return std::nullopt;
  }

  if (const auto *boolean = std::get_if<bool>(value)) {
    return *boolean;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<std::int64_t> integral_value(const Value &value) {
  if (const auto *integer = std::get_if<std::int64_t>(&value)) {
    return *integer;
  }
  if (const auto *boolean = std::get_if<bool>(&value)) {
    return *boolean ? 1 : 0;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<int> compare_values(const Value &left, const Value &right) {
  if (is_null(left) || is_null(right)) {
    return std::nullopt;
  }

  const auto left_integral = integral_value(left);
  const auto right_integral = integral_value(right);
  if (left_integral.has_value() && right_integral.has_value()) {
    if (*left_integral < *right_integral) {
      return -1;
    }
    if (*right_integral < *left_integral) {
      return 1;
    }
    return 0;
  }

  const auto *left_string = std::get_if<std::string>(&left);
  const auto *right_string = std::get_if<std::string>(&right);
  if (left_string != nullptr && right_string != nullptr) {
    if (*left_string < *right_string) {
      return -1;
    }
    if (*right_string < *left_string) {
      return 1;
    }
    return 0;
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
    if (is_null(left) || is_null(right)) {
      return false;
    }
    if (const auto comparison = compare_values(left, right)) {
      return *comparison == 0;
    }
    return std::nullopt;
  case BinaryOperator::NotEqual:
    if (is_null(left) || is_null(right)) {
      return false;
    }
    if (const auto comparison = compare_values(left, right)) {
      return *comparison != 0;
    }
    return std::nullopt;
  case BinaryOperator::Less:
    if (const auto comparison = compare_values(left, right)) {
      return *comparison < 0;
    }
    return std::nullopt;
  case BinaryOperator::LessEqual:
    if (const auto comparison = compare_values(left, right)) {
      return *comparison <= 0;
    }
    return std::nullopt;
  case BinaryOperator::Greater:
    if (const auto comparison = compare_values(left, right)) {
      return *comparison > 0;
    }
    return std::nullopt;
  case BinaryOperator::GreaterEqual:
    if (const auto comparison = compare_values(left, right)) {
      return *comparison >= 0;
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

[[nodiscard]] Result<BoundExpressionPtr> fold_expression(BoundExpressionPtr expression);

[[nodiscard]] Result<BoundExpressionPtr>
fold_unary_expression(std::unique_ptr<BoundUnaryExpression> expression) {
  if (expression->operand == nullptr) {
    return error_result<BoundExpressionPtr>(ErrorCode::PlanError,
                                            "unary expression is missing its operand");
  }

  auto operand = fold_expression(std::move(expression->operand));
  if (!status_ok(operand.status)) {
    return operand;
  }
  expression->operand = std::move(*operand.value);

  if (const auto *operand_value = literal_value(*expression->operand)) {
    if (const auto folded = fold_unary(expression->op, *operand_value)) {
      return ok_result(make_literal(*folded));
    }
  }

  return ok_result<BoundExpressionPtr>(std::move(expression));
}

[[nodiscard]] Result<BoundExpressionPtr>
fold_binary_expression(std::unique_ptr<BoundBinaryExpression> expression) {
  if (expression->left == nullptr || expression->right == nullptr) {
    return error_result<BoundExpressionPtr>(ErrorCode::PlanError,
                                            "binary expression is missing an operand");
  }

  auto left = fold_expression(std::move(expression->left));
  if (!status_ok(left.status)) {
    return left;
  }
  expression->left = std::move(*left.value);

  auto right = fold_expression(std::move(expression->right));
  if (!status_ok(right.status)) {
    return right;
  }
  expression->right = std::move(*right.value);

  const auto *left_value = literal_value(*expression->left);
  const auto *right_value = literal_value(*expression->right);
  if (left_value != nullptr && right_value != nullptr) {
    if (const auto folded = fold_binary(expression->op, *left_value, *right_value)) {
      return ok_result(make_literal(*folded));
    }
  }

  return ok_result<BoundExpressionPtr>(std::move(expression));
}

[[nodiscard]] Result<BoundExpressionPtr>
fold_expression(BoundExpressionPtr expression) {
  if (expression == nullptr) {
    return error_result<BoundExpressionPtr>(ErrorCode::PlanError,
                                            "expression cannot be null");
  }

  if (dynamic_cast<BoundLiteralExpression *>(expression.get()) != nullptr ||
      dynamic_cast<BoundColumnExpression *>(expression.get()) != nullptr) {
    return ok_result(std::move(expression));
  }
  if (dynamic_cast<BoundStarExpression *>(expression.get()) != nullptr) {
    return error_result<BoundExpressionPtr>(
        ErrorCode::PlanError, "star expressions must be expanded before optimization");
  }
  if (auto *unary = dynamic_cast<BoundUnaryExpression *>(expression.get())) {
    (void)unary;
    auto owned = std::unique_ptr<BoundUnaryExpression>(
        static_cast<BoundUnaryExpression *>(expression.release()));
    return fold_unary_expression(std::move(owned));
  }
  if (auto *binary = dynamic_cast<BoundBinaryExpression *>(expression.get())) {
    (void)binary;
    auto owned = std::unique_ptr<BoundBinaryExpression>(
        static_cast<BoundBinaryExpression *>(expression.release()));
    return fold_binary_expression(std::move(owned));
  }

  return error_result<BoundExpressionPtr>(ErrorCode::PlanError,
                                          "unsupported bound expression");
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
