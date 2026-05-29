#pragma once

#include "mattsql/common/status.hpp"
#include "mattsql/common/types.hpp"

namespace mattsql {

enum class TransactionMode {
    ReadOnly,
    ReadWrite
};

enum class TransactionState {
    Active,
    Committed,
    RolledBack,
    Aborted
};

struct TransactionOptions {
    TransactionMode mode = TransactionMode::ReadWrite;
};

class Transaction {
public:
    /// Destroys a transaction through the interface pointer.
    virtual ~Transaction() = default;

    /// Returns the transaction identifier.
    virtual TransactionId Id() const = 0;

    /// Returns the transaction's read/write mode.
    virtual TransactionMode Mode() const = 0;

    /// Returns the current transaction state.
    virtual TransactionState State() const = 0;

    /// Returns the first WAL sequence number owned by this transaction.
    virtual LogSequenceNumber BeginLsn() const = 0;
};

} // namespace mattsql
