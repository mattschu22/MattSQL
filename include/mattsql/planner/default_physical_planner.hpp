#pragma once

#include "mattsql/planner/planner.hpp"

namespace mattsql {

class DefaultPhysicalPlanner final : public PhysicalPlanner {
public:
  /// Lowers logical operators into executable physical operators and attaches
  /// storage access metadata for heap-backed table operations.
  Result<PhysicalPlanPtr> Plan(const LogicalPlan &plan) override;
};

} // namespace mattsql
