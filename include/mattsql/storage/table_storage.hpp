#pragma once

#include "mattsql/catalog/schema.hpp"
#include "mattsql/catalog/table_info.hpp"
#include "mattsql/common/status.hpp"
#include "mattsql/common/types.hpp"
#include "mattsql/storage/heap/heap_table.hpp"
#include "mattsql/txn/transaction.hpp"

#include <memory>
#include <string>

namespace mattsql {

enum class TableStorageMethod { Heap };

struct TableStorageReference {
  TableStorageMethod method = TableStorageMethod::Heap;
  TableId table_id = 0;
  std::string table_name;
  PageId root_page_id = kInvalidPageId;
  TableSchema schema;
};

class TableStorageManager {
public:
  /// Destroys a table storage manager through the interface pointer.
  virtual ~TableStorageManager() = default;

  /// Creates the storage root for a catalog table. Implementations may back
  /// this with hosted files, simulated block devices, or a raw OS-owned device.
  virtual Result<PageId> CreateHeap(Transaction &transaction,
                                    const TableInfo &table) = 0;

  /// Opens a heap table by stable table/page identifiers without depending on a
  /// host filesystem path. The physical executor owns the returned table object.
  virtual Result<std::unique_ptr<HeapTable>>
  OpenHeap(const TableStorageReference &reference) = 0;
};

} // namespace mattsql
