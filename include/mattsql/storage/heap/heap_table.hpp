#pragma once

#include "mattsql/common/status.hpp"
#include "mattsql/common/types.hpp"
#include "mattsql/storage/page/slotted_page.hpp"
#include "mattsql/txn/transaction.hpp"

#include <memory>

namespace mattsql {

class HeapCursor {
public:
    /// Destroys a heap cursor through the interface pointer.
    virtual ~HeapCursor() = default;

    /// Advances to the next visible heap record.
    virtual Result<RecordView> Next() = 0;
};

class HeapTable {
public:
    /// Destroys a heap table through the interface pointer.
    virtual ~HeapTable() = default;

    /// Inserts a serialized row into the heap table.
    virtual Result<RecordId> Insert(Transaction& transaction,
                                    ConstBufferView record) = 0;

    /// Reads a serialized row by record identifier.
    virtual Result<RecordView> Read(Transaction& transaction,
                                    RecordId record_id) = 0;

    /// Deletes a row by record identifier.
    virtual Status Delete(Transaction& transaction, RecordId record_id) = 0;

    /// Opens a sequential scan over visible heap records.
    virtual Result<std::unique_ptr<HeapCursor>> Scan(Transaction& transaction) = 0;
};

} // namespace mattsql
