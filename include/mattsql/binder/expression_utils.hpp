#pragma once

#include "mattsql/binder/bound_expression.hpp"
#include "mattsql/catalog/table_info.hpp"
#include "mattsql/common/status.hpp"

#include <functional>
#include <string_view>
#include <vector>

namespace mattsql {

using BoundExpressionVisitor = std::function<Status(const BoundExpression &expression)>;
using BoundExpressionTransform =
    std::function<Result<BoundExpressionPtr>(BoundExpressionPtr expression)>;

[[nodiscard]] BoundExpressionPtr MakeBoundLiteral(Value value);
[[nodiscard]] BoundExpressionPtr MakeBoundLiteral(Value value, SqlType type);
[[nodiscard]] BoundExpressionPtr MakeBoundColumn(const TableInfo &table,
                                                 const ColumnSchema &column);

[[nodiscard]] const Value *BoundLiteralValue(const BoundExpression &expression);

[[nodiscard]] Result<BoundExpressionPtr>
CloneBoundExpression(const BoundExpression &expression);

[[nodiscard]] Result<std::vector<BoundExpressionPtr>>
CloneBoundExpressions(const std::vector<BoundExpressionPtr> &expressions);

[[nodiscard]] Result<std::vector<std::vector<BoundExpressionPtr>>>
CloneBoundExpressionRows(const std::vector<std::vector<BoundExpressionPtr>> &rows);

[[nodiscard]] Status WalkBoundExpression(const BoundExpression &expression,
                                         const BoundExpressionVisitor &visitor);

[[nodiscard]] Result<BoundExpressionPtr>
TransformBoundExpression(BoundExpressionPtr expression,
                         const BoundExpressionTransform &transform);

[[nodiscard]] Status ValidateNoColumnReferences(const BoundExpression &expression);

[[nodiscard]] Status
ValidateNoColumnReferences(const std::vector<BoundExpressionPtr> &expressions);

} // namespace mattsql
