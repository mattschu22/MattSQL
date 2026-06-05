#pragma once

#include "mattsql/common/types.hpp"
#include "mattsql/parser/ast.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace mattsql {

enum class BoundExpressionKind { Unknown, Literal, Column, Unary, Binary, Star };

struct BoundExpression {
  /// Destroys a bound expression through a base pointer.
  virtual ~BoundExpression() = default;

  [[nodiscard]] virtual BoundExpressionKind Kind() const {
    return BoundExpressionKind::Unknown;
  }

  SqlType type = SqlType::Null;
};

using BoundExpressionPtr = std::unique_ptr<BoundExpression>;

struct BoundLiteralExpression final : BoundExpression {
  [[nodiscard]] BoundExpressionKind Kind() const override {
    return BoundExpressionKind::Literal;
  }

  Value value;
};

struct BoundColumnExpression final : BoundExpression {
  [[nodiscard]] BoundExpressionKind Kind() const override {
    return BoundExpressionKind::Column;
  }

  TableId table_id = 0;
  ColumnId column_id = 0;
  std::string table_name;
  std::string column_name;
};

struct BoundUnaryExpression final : BoundExpression {
  [[nodiscard]] BoundExpressionKind Kind() const override {
    return BoundExpressionKind::Unary;
  }

  UnaryOperator op = UnaryOperator::Plus;
  BoundExpressionPtr operand;
};

struct BoundBinaryExpression final : BoundExpression {
  [[nodiscard]] BoundExpressionKind Kind() const override {
    return BoundExpressionKind::Binary;
  }

  BoundExpressionPtr left;
  BinaryOperator op = BinaryOperator::Add;
  BoundExpressionPtr right;
};

struct BoundStarExpression final : BoundExpression {
  [[nodiscard]] BoundExpressionKind Kind() const override {
    return BoundExpressionKind::Star;
  }
};

} // namespace mattsql
