#pragma once

#include "mattsql/binder/bound_expression.hpp"
#include "mattsql/catalog/table_info.hpp"

#include <memory>
#include <string>
#include <vector>

namespace mattsql {

enum class BoundStatementKind { Unknown, Select, Insert, CreateTable };

struct BoundStatement {
  /// Destroys a bound statement through a base pointer.
  virtual ~BoundStatement() = default;

  [[nodiscard]] virtual BoundStatementKind Kind() const {
    return BoundStatementKind::Unknown;
  }
};

using BoundStatementPtr = std::unique_ptr<BoundStatement>;

struct BoundSelectStatement final : BoundStatement {
  [[nodiscard]] BoundStatementKind Kind() const override {
    return BoundStatementKind::Select;
  }

  std::vector<BoundExpressionPtr> projections;
  std::vector<std::string> projection_names;
  TableInfo table;
  BoundExpressionPtr where;
};

struct BoundInsertStatement final : BoundStatement {
  [[nodiscard]] BoundStatementKind Kind() const override {
    return BoundStatementKind::Insert;
  }

  TableInfo table;
  std::vector<BoundExpressionPtr> values;
};

struct BoundCreateTableStatement final : BoundStatement {
  [[nodiscard]] BoundStatementKind Kind() const override {
    return BoundStatementKind::CreateTable;
  }

  CreateTableRequest request;
};

} // namespace mattsql
