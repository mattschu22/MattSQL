#pragma once

#include "mattsql/common/status.hpp"
#include "mattsql/txn/transaction.hpp"

#include <memory>

namespace mattsql {

class TransactionManager {
public:
    /// Destroys a transaction manager through the interface pointer.
    virtual ~TransactionManager() = default;

    /// Begins a transaction with the requested options.
    virtual Result<std::unique_ptr<Transaction>>
    Begin(const TransactionOptions& options) = 0;

    /// Commits a transaction and makes its effects durable according to policy.
    virtual Status Commit(Transaction& transaction) = 0;

    /// Rolls back a transaction's uncommitted effects.
    virtual Status Rollback(Transaction& transaction) = 0;
};

} // namespace mattsql
