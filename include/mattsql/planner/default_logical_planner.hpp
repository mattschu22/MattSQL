#pragma once

#include "mattsql/planner/planner.hpp"

namespace mattsql {

class DefaultLogicalPlanner final : public LogicalPlanner {
public:
  /// Builds a logical operator tree from a statement that has already passed
  /// catalog name resolution and semantic type binding.
  Result<LogicalPlanPtr> Plan(const BoundStatement &statement) override;
};

} // namespace mattsql
