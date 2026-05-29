#pragma once

#include "mattsql/binder/bound_expression.hpp"
#include "mattsql/catalog/table_info.hpp"
#include "mattsql/common/types.hpp"

#include <memory>
#include <vector>

namespace mattsql {

enum class LogicalOperatorKind {
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

  LogicalOperatorKind kind = LogicalOperatorKind::SeqScan;
  std::vector<std::unique_ptr<LogicalPlan>> children;
};

using LogicalPlanPtr = std::unique_ptr<LogicalPlan>;

struct LogicalSeqScan final : LogicalPlan {
  TableInfo table;
};

struct LogicalFilter final : LogicalPlan {
  BoundExpressionPtr predicate;
};

struct LogicalProjection final : LogicalPlan {
  std::vector<BoundExpressionPtr> projections;
};

struct LogicalValues final : LogicalPlan {
  std::vector<std::vector<BoundExpressionPtr>> rows;
  TupleBatch tuples;
};

struct LogicalInsert final : LogicalPlan {
  TableInfo table;
};

struct LogicalCreateTable final : LogicalPlan {
  CreateTableRequest request;
};

} // namespace mattsql
