#pragma once

#include "mattsql/binder/bound_expression.hpp"
#include "mattsql/catalog/table_info.hpp"
#include "mattsql/common/types.hpp"

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
};

struct PhysicalFilter final : PhysicalPlan {
    BoundExpressionPtr predicate;
};

struct PhysicalProjection final : PhysicalPlan {
    std::vector<BoundExpressionPtr> projections;
};

struct PhysicalValues final : PhysicalPlan {
    TupleBatch tuples;
};

struct PhysicalInsert final : PhysicalPlan {
    TableInfo table;
};

struct PhysicalCreateTable final : PhysicalPlan {
    CreateTableRequest request;
};

} // namespace mattsql
