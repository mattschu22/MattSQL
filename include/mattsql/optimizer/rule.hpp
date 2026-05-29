#pragma once

#include "mattsql/common/status.hpp"
#include "mattsql/planner/logical_plan.hpp"

#include <string_view>

namespace mattsql {

class OptimizerRule {
public:
    /// Destroys an optimizer rule through the interface pointer.
    virtual ~OptimizerRule() = default;

    /// Returns the stable rule name used in traces and diagnostics.
    virtual std::string_view Name() const = 0;

    /// Applies the rule to a logical plan and returns the rewritten root.
    virtual Result<LogicalPlanPtr> Apply(LogicalPlanPtr plan) = 0;
};

} // namespace mattsql
