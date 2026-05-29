#pragma once

#include "mattsql/binder/bound_statement.hpp"
#include "mattsql/common/status.hpp"
#include "mattsql/planner/logical_plan.hpp"
#include "mattsql/planner/physical_plan.hpp"

namespace mattsql {

class LogicalPlanner {
public:
    /// Destroys a logical planner through the interface pointer.
    virtual ~LogicalPlanner() = default;

    /// Converts a bound SQL statement into a logical plan tree.
    virtual Result<LogicalPlanPtr> Plan(const BoundStatement& statement) = 0;
};

class PhysicalPlanner {
public:
    /// Destroys a physical planner through the interface pointer.
    virtual ~PhysicalPlanner() = default;

    /// Lowers a logical plan into executable physical operators.
    virtual Result<PhysicalPlanPtr> Plan(const LogicalPlan& plan) = 0;
};

} // namespace mattsql
