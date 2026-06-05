#pragma once

#include "mattsql/binder/bound_expression.hpp"
#include "mattsql/catalog/table_info.hpp"
#include "mattsql/common/types.hpp"
#include "mattsql/storage/table_storage.hpp"

#include <memory>
#include <vector>

namespace mattsql {

enum class PhysicalOperatorKind {
  Unknown,
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

  [[nodiscard]] virtual PhysicalOperatorKind Kind() const {
    return PhysicalOperatorKind::Unknown;
  }

  std::vector<std::unique_ptr<PhysicalPlan>> children;
};

using PhysicalPlanPtr = std::unique_ptr<PhysicalPlan>;

struct PhysicalSeqScan final : PhysicalPlan {
  [[nodiscard]] PhysicalOperatorKind Kind() const override {
    return PhysicalOperatorKind::SeqScan;
  }

  TableInfo table;
  TableStorageReference storage;
};

struct PhysicalFilter final : PhysicalPlan {
  [[nodiscard]] PhysicalOperatorKind Kind() const override {
    return PhysicalOperatorKind::Filter;
  }

  BoundExpressionPtr predicate;
};

struct PhysicalProjection final : PhysicalPlan {
  [[nodiscard]] PhysicalOperatorKind Kind() const override {
    return PhysicalOperatorKind::Projection;
  }

  std::vector<BoundExpressionPtr> projections;
  std::vector<std::string> projection_names;
};

struct PhysicalValues final : PhysicalPlan {
  [[nodiscard]] PhysicalOperatorKind Kind() const override {
    return PhysicalOperatorKind::Values;
  }

  std::vector<std::vector<BoundExpressionPtr>> rows;
  TupleBatch tuples;
};

struct PhysicalInsert final : PhysicalPlan {
  [[nodiscard]] PhysicalOperatorKind Kind() const override {
    return PhysicalOperatorKind::Insert;
  }

  TableInfo table;
  TableStorageReference storage;
};

struct PhysicalCreateTable final : PhysicalPlan {
  [[nodiscard]] PhysicalOperatorKind Kind() const override {
    return PhysicalOperatorKind::CreateTable;
  }

  CreateTableRequest request;
  TableStorageMethod storage_method = TableStorageMethod::Heap;
};

} // namespace mattsql
