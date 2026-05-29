#pragma once

#include "mattsql/common/status.hpp"
#include "mattsql/common/types.hpp"
#include "mattsql/storage/page/slotted_page.hpp"
#include "mattsql/txn/transaction.hpp"

#include <memory>
#include <vector>

namespace mattsql {

struct IndexKey {
    std::vector<std::byte> bytes;
};

class IndexCursor {
public:
    /// Destroys an index cursor through the interface pointer.
    virtual ~IndexCursor() = default;

    /// Advances to the next record identifier matching the scan.
    virtual Result<RecordId> Next() = 0;
};

class BTreeIndex {
public:
    /// Destroys a B+ tree index through the interface pointer.
    virtual ~BTreeIndex() = default;

    /// Inserts a key-to-record mapping.
    virtual Status Insert(Transaction& transaction, const IndexKey& key,
                          RecordId record_id) = 0;

    /// Deletes a key-to-record mapping.
    virtual Status Delete(Transaction& transaction, const IndexKey& key,
                          RecordId record_id) = 0;

    /// Looks up one key and returns a cursor over matching record identifiers.
    virtual Result<std::unique_ptr<IndexCursor>> Seek(Transaction& transaction,
                                                      const IndexKey& key) = 0;
};

} // namespace mattsql
