#pragma once

#include "mattsql/binder/bound_expression.hpp"
#include "mattsql/catalog/table_info.hpp"
#include "mattsql/common/types.hpp"

#include <memory>
#include <vector>

namespace mattsql {

enum class LogicalOperatorKind {
  Unknown,
  CreateTable,
  Insert,
  Projection,
  Filter,
  Values,
  SeqScan
};

struct LogicalPlan {
  /// Destroys a logical plan node through a base pointer.
  virtual ~LogicalPlan() = default;

  [[nodiscard]] virtual LogicalOperatorKind Kind() const {
    return LogicalOperatorKind::Unknown;
  }

  std::vector<std::unique_ptr<LogicalPlan>> children;
};

using LogicalPlanPtr = std::unique_ptr<LogicalPlan>;

struct LogicalSeqScan final : LogicalPlan {
  [[nodiscard]] LogicalOperatorKind Kind() const override {
    return LogicalOperatorKind::SeqScan;
  }

  TableInfo table;
};

struct LogicalFilter final : LogicalPlan {
  [[nodiscard]] LogicalOperatorKind Kind() const override {
    return LogicalOperatorKind::Filter;
  }

  BoundExpressionPtr predicate;
};

struct LogicalProjection final : LogicalPlan {
  [[nodiscard]] LogicalOperatorKind Kind() const override {
    return LogicalOperatorKind::Projection;
  }

  std::vector<BoundExpressionPtr> projections;
  std::vector<std::string> projection_names;
};

struct LogicalValues final : LogicalPlan {
  [[nodiscard]] LogicalOperatorKind Kind() const override {
    return LogicalOperatorKind::Values;
  }

  std::vector<std::vector<BoundExpressionPtr>> rows;
  TupleBatch tuples;
};

struct LogicalInsert final : LogicalPlan {
  [[nodiscard]] LogicalOperatorKind Kind() const override {
    return LogicalOperatorKind::Insert;
  }

  TableInfo table;
};

struct LogicalCreateTable final : LogicalPlan {
  [[nodiscard]] LogicalOperatorKind Kind() const override {
    return LogicalOperatorKind::CreateTable;
  }

  CreateTableRequest request;
};

} // namespace mattsql
