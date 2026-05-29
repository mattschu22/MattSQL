#pragma once

#include "mattsql/binder/bound_expression.hpp"
#include "mattsql/catalog/schema.hpp"
#include "mattsql/common/status.hpp"
#include "mattsql/common/types.hpp"

namespace mattsql {

struct EvaluationContext {
    TupleView tuple;
    const TableSchema* schema = nullptr;
};

class ExpressionEvaluator {
public:
    /// Destroys an expression evaluator through the interface pointer.
    virtual ~ExpressionEvaluator() = default;

    /// Evaluates a bound expression against the provided row context.
    virtual Result<Value> Evaluate(const BoundExpression& expression,
                                   const EvaluationContext& context) = 0;
};

} // namespace mattsql
