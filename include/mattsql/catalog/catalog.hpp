#pragma once

#include "mattsql/catalog/schema.hpp"
#include "mattsql/catalog/table_info.hpp"
#include "mattsql/common/status.hpp"
#include "mattsql/common/types.hpp"

#include <string_view>
#include <vector>

namespace mattsql {

class Catalog {
public:
  /// Destroys a catalog through the interface pointer.
  virtual ~Catalog() = default;

  /// Creates a table and returns its assigned metadata.
  virtual Result<TableInfo> CreateTable(const CreateTableRequest &request) = 0;

  /// Drops a table and all catalog metadata owned by that table.
  virtual Status DropTable(std::string_view table_name) = 0;

  /// Looks up a table by name.
  virtual Result<TableInfo> GetTable(std::string_view table_name) const = 0;

  /// Looks up a table by stable catalog identifier.
  virtual Result<TableInfo> GetTable(TableId table_id) const = 0;

  /// Updates the storage root page for a table after physical storage exists.
  virtual Status SetTableHeapRoot(TableId table_id, PageId heap_root_page_id) = 0;

  /// Returns all user-visible tables.
  virtual Result<std::vector<TableInfo>> ListTables() const = 0;

  /// Creates an index over an existing table.
  virtual Result<IndexInfo> CreateIndex(const CreateIndexRequest &request) = 0;

  /// Looks up an index by table and index name.
  virtual Result<IndexInfo> GetIndex(std::string_view table_name,
                                     std::string_view index_name) const = 0;
};

} // namespace mattsql
