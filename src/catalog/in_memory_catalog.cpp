#include "mattsql/catalog/in_memory_catalog.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>

namespace mattsql {
namespace {

[[nodiscard]] Status ok_status() { return {}; }

[[nodiscard]] Status error_status(ErrorCode code, std::string message) {
  return Status{code, std::move(message)};
}

template <typename T> [[nodiscard]] Result<T> ok_result(T value) {
  Result<T> result;
  result.value.emplace(std::move(value));
  return result;
}

template <typename T>
[[nodiscard]] Result<T> error_result(ErrorCode code, std::string message) {
  Result<T> result;
  result.status = error_status(code, std::move(message));
  return result;
}

[[nodiscard]] std::string catalog_key(std::string_view value) {
  std::string key;
  key.reserve(value.size());

  // SQL identifiers are treated case-insensitively in this hosted catalog while
  // the original spelling is preserved in returned TableInfo/IndexInfo values.
  for (const char character : value) {
    key.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
  }

  return key;
}

[[nodiscard]] Status validate_column(const ColumnSchema &column) {
  if (column.name.empty()) {
    return error_status(ErrorCode::InvalidArgument, "column name is required");
  }
  if (column.type == SqlType::Null) {
    return error_status(ErrorCode::InvalidArgument, "column type cannot be Null");
  }

  return ok_status();
}

} // namespace

InMemoryCatalog::InMemoryCatalog()
    : InMemoryCatalog(std::make_unique<InMemoryHostedCatalogApi>()) {}

InMemoryCatalog::InMemoryCatalog(std::unique_ptr<HostedCatalogApi> hosted_api)
    : hosted_api_(std::move(hosted_api)) {
  if (!hosted_api_) {
    hosted_api_ = std::make_unique<InMemoryHostedCatalogApi>();
  }
}

InMemoryCatalog::~InMemoryCatalog() = default;

InMemoryCatalog::InMemoryCatalog(InMemoryCatalog &&) noexcept = default;

InMemoryCatalog &InMemoryCatalog::operator=(InMemoryCatalog &&) noexcept = default;

Result<TableInfo> InMemoryCatalog::CreateTable(const CreateTableRequest &request) {
  if (request.name.empty()) {
    return error_result<TableInfo>(ErrorCode::InvalidArgument,
                                   "table name is required");
  }
  if (request.schema.columns.empty()) {
    return error_result<TableInfo>(ErrorCode::InvalidArgument,
                                   "table must have at least one column");
  }

  const auto table_key = catalog_key(request.name);
  const auto existing_table = hosted_api_->LoadTable(table_key);
  if (existing_table.status.code == ErrorCode::Ok) {
    return error_result<TableInfo>(ErrorCode::AlreadyExists, "table already exists");
  }
  if (existing_table.status.code != ErrorCode::NotFound) {
    return error_result<TableInfo>(existing_table.status.code,
                                   existing_table.status.message);
  }

  TableInfo table;
  table.name = request.name;

  std::unordered_set<std::string> column_names;
  table.schema.columns.reserve(request.schema.columns.size());

  // Validate the full schema before assigning an ID so failed requests do not
  // leave holes in the hosted catalog's table-id sequence.
  for (std::size_t index = 0; index < request.schema.columns.size(); ++index) {
    if (index > std::numeric_limits<ColumnId>::max()) {
      return error_result<TableInfo>(ErrorCode::InvalidArgument,
                                     "table has too many columns");
    }

    auto column = request.schema.columns[index];
    const auto column_status = validate_column(column);
    if (column_status.code != ErrorCode::Ok) {
      return error_result<TableInfo>(column_status.code, column_status.message);
    }

    const auto column_key = catalog_key(column.name);
    if (!column_names.insert(column_key).second) {
      return error_result<TableInfo>(ErrorCode::AlreadyExists, "duplicate column name");
    }

    column.id = static_cast<ColumnId>(index);
    table.schema.columns.push_back(std::move(column));
  }

  const auto table_id = hosted_api_->AllocateTableId();
  if (table_id.status.code != ErrorCode::Ok) {
    return error_result<TableInfo>(table_id.status.code, table_id.status.message);
  }

  table.id = *table_id.value;
  return hosted_api_->StoreTable(table_key, table);
}

Status InMemoryCatalog::DropTable(std::string_view table_name) {
  const auto table_key = catalog_key(table_name);
  return hosted_api_->EraseTable(table_key);
}

Result<TableInfo> InMemoryCatalog::GetTable(std::string_view table_name) const {
  const auto table_key = catalog_key(table_name);
  return hosted_api_->LoadTable(table_key);
}

Result<TableInfo> InMemoryCatalog::GetTable(TableId table_id) const {
  return hosted_api_->LoadTable(table_id);
}

Result<std::vector<TableInfo>> InMemoryCatalog::ListTables() const {
  return hosted_api_->LoadTables();
}

Result<IndexInfo> InMemoryCatalog::CreateIndex(const CreateIndexRequest &request) {
  if (request.table_name.empty()) {
    return error_result<IndexInfo>(ErrorCode::InvalidArgument,
                                   "table name is required");
  }
  if (request.schema.name.empty()) {
    return error_result<IndexInfo>(ErrorCode::InvalidArgument,
                                   "index name is required");
  }
  if (request.schema.key_columns.empty()) {
    return error_result<IndexInfo>(ErrorCode::InvalidArgument,
                                   "index must have at least one key column");
  }

  const auto table_key = catalog_key(request.table_name);
  const auto table = hosted_api_->LoadTable(table_key);
  if (table.status.code != ErrorCode::Ok) {
    return error_result<IndexInfo>(table.status.code, table.status.message);
  }

  const auto index_key = catalog_key(request.schema.name);
  const auto existing_index = hosted_api_->LoadIndex(table_key, index_key);
  if (existing_index.status.code == ErrorCode::Ok) {
    return error_result<IndexInfo>(ErrorCode::AlreadyExists, "index already exists");
  }
  if (existing_index.status.code != ErrorCode::NotFound) {
    return error_result<IndexInfo>(existing_index.status.code,
                                   existing_index.status.message);
  }

  std::unordered_set<ColumnId> key_columns;
  for (const auto column_id : request.schema.key_columns) {
    if (column_id >= table.value->schema.columns.size()) {
      return error_result<IndexInfo>(ErrorCode::InvalidArgument,
                                     "index column is out of range");
    }
    if (!key_columns.insert(column_id).second) {
      return error_result<IndexInfo>(ErrorCode::InvalidArgument,
                                     "duplicate index key column");
    }
  }

  IndexInfo index;
  const auto index_id = hosted_api_->AllocateIndexId();
  if (index_id.status.code != ErrorCode::Ok) {
    return error_result<IndexInfo>(index_id.status.code, index_id.status.message);
  }

  index.id = *index_id.value;
  index.name = request.schema.name;
  index.table_id = table.value->id;
  index.schema = request.schema;
  // root_page_id stays kInvalidPageId here; physical index pages are assigned by
  // a future storage-backed catalog, not by this hosted metadata store.

  return hosted_api_->StoreIndex(table_key, index_key, index);
}

Result<IndexInfo> InMemoryCatalog::GetIndex(std::string_view table_name,
                                            std::string_view index_name) const {
  const auto table_key = catalog_key(table_name);
  const auto index_key = catalog_key(index_name);
  return hosted_api_->LoadIndex(table_key, index_key);
}

} // namespace mattsql
