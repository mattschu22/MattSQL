#pragma once

#include "mattsql/common/types.hpp"
#include "mattsql/parser/ast.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace mattsql {

enum class BoundExpressionKind {
    Literal,
    Column,
    Unary,
    Binary,
    Star
};

struct BoundExpression {
    /// Destroys a bound expression through a base pointer.
    virtual ~BoundExpression() = default;

    BoundExpressionKind kind = BoundExpressionKind::Literal;
    SqlType type = SqlType::Null;
};

using BoundExpressionPtr = std::unique_ptr<BoundExpression>;

struct BoundLiteralExpression final : BoundExpression {
    Value value;
};

struct BoundColumnExpression final : BoundExpression {
    TableId table_id = 0;
    ColumnId column_id = 0;
    std::string table_name;
    std::string column_name;
};

struct BoundUnaryExpression final : BoundExpression {
    UnaryOperator op = UnaryOperator::Plus;
    BoundExpressionPtr operand;
};

struct BoundBinaryExpression final : BoundExpression {
    BoundExpressionPtr left;
    BinaryOperator op = BinaryOperator::Add;
    BoundExpressionPtr right;
};

struct BoundStarExpression final : BoundExpression {};

} // namespace mattsql
