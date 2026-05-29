#pragma once

#include "mattsql/catalog/table_info.hpp"
#include "mattsql/common/status.hpp"

#include <memory>
#include <string_view>
#include <vector>

namespace mattsql {

// Narrow hosted metadata API used by InMemoryCatalog. The default implementation
// is process-local; a later storage-backed API can persist the same metadata.
class HostedCatalogApi {
public:
  virtual ~HostedCatalogApi() = default;

  virtual Result<TableId> AllocateTableId() = 0;
  virtual Result<IndexId> AllocateIndexId() = 0;

  virtual Result<TableInfo> StoreTable(std::string_view table_key,
                                       const TableInfo &table) = 0;
  virtual Status EraseTable(std::string_view table_key) = 0;
  virtual Status UpdateTable(const TableInfo &table) = 0;
  virtual Result<TableInfo> LoadTable(std::string_view table_key) const = 0;
  virtual Result<TableInfo> LoadTable(TableId table_id) const = 0;
  virtual Result<std::vector<TableInfo>> LoadTables() const = 0;

  virtual Result<IndexInfo> StoreIndex(std::string_view table_key,
                                       std::string_view index_key,
                                       const IndexInfo &index) = 0;
  virtual Result<IndexInfo> LoadIndex(std::string_view table_key,
                                      std::string_view index_key) const = 0;
};

class InMemoryHostedCatalogApi final : public HostedCatalogApi {
public:
  InMemoryHostedCatalogApi();
  ~InMemoryHostedCatalogApi() override;

  InMemoryHostedCatalogApi(const InMemoryHostedCatalogApi &) = delete;
  InMemoryHostedCatalogApi &operator=(const InMemoryHostedCatalogApi &) = delete;

  InMemoryHostedCatalogApi(InMemoryHostedCatalogApi &&) noexcept;
  InMemoryHostedCatalogApi &operator=(InMemoryHostedCatalogApi &&) noexcept;

  Result<TableId> AllocateTableId() override;
  Result<IndexId> AllocateIndexId() override;

  Result<TableInfo> StoreTable(std::string_view table_key,
                               const TableInfo &table) override;
  Status EraseTable(std::string_view table_key) override;
  Status UpdateTable(const TableInfo &table) override;
  Result<TableInfo> LoadTable(std::string_view table_key) const override;
  Result<TableInfo> LoadTable(TableId table_id) const override;
  Result<std::vector<TableInfo>> LoadTables() const override;

  Result<IndexInfo> StoreIndex(std::string_view table_key, std::string_view index_key,
                               const IndexInfo &index) override;
  Result<IndexInfo> LoadIndex(std::string_view table_key,
                              std::string_view index_key) const override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace mattsql
