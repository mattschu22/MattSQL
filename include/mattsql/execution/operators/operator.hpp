#pragma once

#include "mattsql/common/status.hpp"
#include "mattsql/common/types.hpp"
#include "mattsql/txn/transaction.hpp"

namespace mattsql {

class PhysicalOperator {
public:
    /// Destroys a physical operator through the interface pointer.
    virtual ~PhysicalOperator() = default;

    /// Prepares the operator to produce rows inside a transaction.
    virtual Status Open(Transaction& transaction) = 0;

    /// Produces the next columnar batch from the operator.
    virtual Result<VectorBatch> Next() = 0;

    /// Releases resources held by the operator.
    virtual Status Close() = 0;
};

} // namespace mattsql
