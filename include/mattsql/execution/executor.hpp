#pragma once

#include "mattsql/common/status.hpp"
#include "mattsql/engine.hpp"
#include "mattsql/planner/physical_plan.hpp"
#include "mattsql/txn/transaction.hpp"

namespace mattsql {

class Executor {
public:
    /// Destroys an executor through the interface pointer.
    virtual ~Executor() = default;

    /// Executes a physical plan and materializes the query result.
    virtual Result<QueryResult> Execute(const PhysicalPlan& plan,
                                        Transaction& transaction) = 0;
};

} // namespace mattsql
