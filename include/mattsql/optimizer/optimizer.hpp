#pragma once

#include "mattsql/common/status.hpp"
#include "mattsql/optimizer/rule.hpp"
#include "mattsql/planner/logical_plan.hpp"

#include <memory>

namespace mattsql {

class Optimizer {
public:
    /// Destroys an optimizer through the interface pointer.
    virtual ~Optimizer() = default;

    /// Adds a logical rewrite rule to the optimizer pipeline.
    virtual Status AddRule(std::unique_ptr<OptimizerRule> rule) = 0;

    /// Optimizes a logical plan and returns the rewritten root.
    virtual Result<LogicalPlanPtr> Optimize(LogicalPlanPtr plan) = 0;
};

} // namespace mattsql
