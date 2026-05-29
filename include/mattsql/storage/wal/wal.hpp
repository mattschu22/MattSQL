#pragma once

#include "mattsql/common/status.hpp"
#include "mattsql/common/types.hpp"
#include "mattsql/storage/page/slotted_page.hpp"

#include <cstdint>

namespace mattsql {

enum class WalRecordType {
    Begin,
    Commit,
    Rollback,
    Insert,
    Delete,
    Update,
    Checkpoint
};

struct WalRecord {
    WalRecordType type = WalRecordType::Begin;
    TransactionId transaction_id = 0;
    PageId page_id = kInvalidPageId;
    RecordId record_id;
    ConstBufferView payload;
};

class WalManager {
public:
    /// Destroys a WAL manager through the interface pointer.
    virtual ~WalManager() = default;

    /// Appends a WAL record and returns its assigned LSN.
    virtual Result<LogSequenceNumber> Append(const WalRecord& record) = 0;

    /// Forces WAL records through the provided LSN to durable media.
    virtual Status Flush(LogSequenceNumber lsn) = 0;

    /// Returns the greatest LSN known to be durable.
    virtual LogSequenceNumber DurableLsn() const = 0;

    /// Starts recovery over durable WAL records.
    virtual Status Recover() = 0;
};

} // namespace mattsql
