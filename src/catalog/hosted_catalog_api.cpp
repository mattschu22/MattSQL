#include "mattsql/catalog/hosted_catalog_api.hpp"

#include "mattsql/common/result_utils.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>

namespace mattsql {

struct InMemoryHostedCatalogApi::Impl {
  TableId next_table_id = 1;
  IndexId next_index_id = 1;

  std::unordered_map<std::string, TableInfo> tables_by_name;
  std::unordered_map<TableId, std::string> table_names_by_id;
  std::unordered_map<std::string, std::unordered_map<std::string, IndexInfo>>
      indexes_by_table_name;
};

InMemoryHostedCatalogApi::InMemoryHostedCatalogApi()
    : impl_(std::make_unique<Impl>()) {}

InMemoryHostedCatalogApi::~InMemoryHostedCatalogApi() = default;

InMemoryHostedCatalogApi::InMemoryHostedCatalogApi(
    InMemoryHostedCatalogApi &&) noexcept = default;

InMemoryHostedCatalogApi &
InMemoryHostedCatalogApi::operator=(InMemoryHostedCatalogApi &&) noexcept = default;

Result<TableId> InMemoryHostedCatalogApi::AllocateTableId() {
  return ok_result(impl_->next_table_id++);
}

Result<IndexId> InMemoryHostedCatalogApi::AllocateIndexId() {
  return ok_result(impl_->next_index_id++);
}

Result<TableInfo> InMemoryHostedCatalogApi::StoreTable(std::string_view table_key,
                                                       const TableInfo &table) {
  const std::string key(table_key);
  if (impl_->tables_by_name.contains(key)) {
    return error_result<TableInfo>(ErrorCode::AlreadyExists, "table already exists");
  }

  impl_->table_names_by_id.emplace(table.id, key);
  impl_->tables_by_name.emplace(key, table);
  return ok_result(table);
}

Status InMemoryHostedCatalogApi::EraseTable(std::string_view table_key) {
  const std::string key(table_key);
  const auto table_iter = impl_->tables_by_name.find(key);
  if (table_iter == impl_->tables_by_name.end()) {
    return error_status(ErrorCode::NotFound, "table not found");
  }

  impl_->table_names_by_id.erase(table_iter->second.id);
  impl_->indexes_by_table_name.erase(key);
  impl_->tables_by_name.erase(table_iter);
  return ok_status();
}

Status InMemoryHostedCatalogApi::UpdateTable(const TableInfo &table) {
  const auto name_iter = impl_->table_names_by_id.find(table.id);
  if (name_iter == impl_->table_names_by_id.end()) {
    return error_status(ErrorCode::NotFound, "table not found");
  }

  const auto table_iter = impl_->tables_by_name.find(name_iter->second);
  if (table_iter == impl_->tables_by_name.end()) {
    return error_status(ErrorCode::Corruption, "table id index is corrupt");
  }

  auto updated = table;
  updated.name = table_iter->second.name;
  table_iter->second = std::move(updated);
  return ok_status();
}

Result<TableInfo>
InMemoryHostedCatalogApi::LoadTable(std::string_view table_key) const {
  const auto table_iter = impl_->tables_by_name.find(std::string(table_key));
  if (table_iter == impl_->tables_by_name.end()) {
    return error_result<TableInfo>(ErrorCode::NotFound, "table not found");
  }

  return ok_result(table_iter->second);
}

Result<TableInfo> InMemoryHostedCatalogApi::LoadTable(TableId table_id) const {
  const auto name_iter = impl_->table_names_by_id.find(table_id);
  if (name_iter == impl_->table_names_by_id.end()) {
    return error_result<TableInfo>(ErrorCode::NotFound, "table not found");
  }

  return ok_result(impl_->tables_by_name.at(name_iter->second));
}

Result<std::vector<TableInfo>> InMemoryHostedCatalogApi::LoadTables() const {
  std::vector<TableInfo> tables;
  tables.reserve(impl_->tables_by_name.size());

  for (const auto &[name, table] : impl_->tables_by_name) {
    (void)name;
    tables.push_back(table);
  }

  std::sort(tables.begin(), tables.end(),
            [](const auto &left, const auto &right) { return left.id < right.id; });

  return ok_result(std::move(tables));
}

Result<IndexInfo> InMemoryHostedCatalogApi::StoreIndex(std::string_view table_key,
                                                       std::string_view index_key,
                                                       const IndexInfo &index) {
  auto table = LoadTable(table_key);
  if (!status_ok(table.status)) {
    return error_result<IndexInfo>(table.status.code, table.status.message);
  }

  const std::string key(index_key);
  auto &indexes = impl_->indexes_by_table_name[std::string(table_key)];
  if (indexes.contains(key)) {
    return error_result<IndexInfo>(ErrorCode::AlreadyExists, "index already exists");
  }

  auto table_iter = impl_->tables_by_name.find(std::string(table_key));
  table_iter->second.indexes.push_back(index);
  indexes.emplace(key, index);
  return ok_result(index);
}

Result<IndexInfo>
InMemoryHostedCatalogApi::LoadIndex(std::string_view table_key,
                                    std::string_view index_key) const {
  auto table = LoadTable(table_key);
  if (!status_ok(table.status)) {
    return error_result<IndexInfo>(table.status.code, table.status.message);
  }

  const std::string key(index_key);
  const auto table_index_iter =
      impl_->indexes_by_table_name.find(std::string(table_key));
  if (table_index_iter == impl_->indexes_by_table_name.end()) {
    return error_result<IndexInfo>(ErrorCode::NotFound, "index not found");
  }

  const auto index_iter = table_index_iter->second.find(key);
  if (index_iter != table_index_iter->second.end()) {
    return ok_result(index_iter->second);
  }

  return error_result<IndexInfo>(ErrorCode::NotFound, "index not found");
}

} // namespace mattsql
