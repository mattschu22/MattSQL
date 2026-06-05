#pragma once

#include "mattsql/binder/bound_expression.hpp"
#include "mattsql/catalog/schema.hpp"
#include "mattsql/common/status.hpp"
#include "mattsql/common/types.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace mattsql {

struct EvaluationContext {
  const TableSchema *schema = nullptr;
  const std::vector<Value> *row = nullptr;
};

class ExpressionEvaluator {
public:
  /// Destroys an expression evaluator through the interface pointer.
  virtual ~ExpressionEvaluator() = default;

  /// Evaluates a bound expression against the provided row context.
  virtual Result<Value> Evaluate(const BoundExpression &expression,
                                 const EvaluationContext &context) = 0;
};

class DefaultExpressionEvaluator final : public ExpressionEvaluator {
public:
  Result<Value> Evaluate(const BoundExpression &expression,
                         const EvaluationContext &context) override;
};

[[nodiscard]] std::string ProjectionName(const BoundExpression &expression,
                                         std::size_t index);

[[nodiscard]] Result<std::vector<Value>>
EvaluateExpressions(ExpressionEvaluator &evaluator,
                    const std::vector<BoundExpressionPtr> &expressions,
                    const EvaluationContext &context);

} // namespace mattsql
