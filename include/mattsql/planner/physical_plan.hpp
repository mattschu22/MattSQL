#pragma once

#include "mattsql/binder/bound_expression.hpp"
#include "mattsql/catalog/table_info.hpp"
#include "mattsql/common/types.hpp"
#include "mattsql/storage/table_storage.hpp"

#include <memory>
#include <vector>

namespace mattsql {

enum class PhysicalOperatorKind {
  CreateTable,
  Insert,
  Projection,
  Filter,
  Values,
  SeqScan
};

struct PhysicalPlan {
  /// Destroys a physical plan node through a base pointer.
  virtual ~PhysicalPlan() = default;

  PhysicalOperatorKind kind = PhysicalOperatorKind::SeqScan;
  std::vector<std::unique_ptr<PhysicalPlan>> children;
};

using PhysicalPlanPtr = std::unique_ptr<PhysicalPlan>;

struct PhysicalSeqScan final : PhysicalPlan {
  TableInfo table;
  TableStorageReference storage;
};

struct PhysicalFilter final : PhysicalPlan {
  BoundExpressionPtr predicate;
};

struct PhysicalProjection final : PhysicalPlan {
  std::vector<BoundExpressionPtr> projections;
  std::vector<std::string> projection_names;
};

struct PhysicalValues final : PhysicalPlan {
  std::vector<std::vector<BoundExpressionPtr>> rows;
  TupleBatch tuples;
};

struct PhysicalInsert final : PhysicalPlan {
  TableInfo table;
  TableStorageReference storage;
};

struct PhysicalCreateTable final : PhysicalPlan {
  CreateTableRequest request;
  TableStorageMethod storage_method = TableStorageMethod::Heap;
};

} // namespace mattsql
