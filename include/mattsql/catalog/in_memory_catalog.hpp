#pragma once

#include "mattsql/catalog/catalog.hpp"
#include "mattsql/catalog/hosted_catalog_api.hpp"

#include <memory>

namespace mattsql {

// Hosted catalog used by the early engine path. It owns metadata in process
// memory only; heap/index page identifiers remain invalid until a persistent
// catalog and storage layer are wired in.
class InMemoryCatalog final : public Catalog {
public:
  InMemoryCatalog();
  explicit InMemoryCatalog(std::unique_ptr<HostedCatalogApi> hosted_api);
  ~InMemoryCatalog() override;

  InMemoryCatalog(const InMemoryCatalog &) = delete;
  InMemoryCatalog &operator=(const InMemoryCatalog &) = delete;

  InMemoryCatalog(InMemoryCatalog &&) noexcept;
  InMemoryCatalog &operator=(InMemoryCatalog &&) noexcept;

  Result<TableInfo> CreateTable(const CreateTableRequest &request) override;
  Status DropTable(std::string_view table_name) override;
  Result<TableInfo> GetTable(std::string_view table_name) const override;
  Result<TableInfo> GetTable(TableId table_id) const override;
  Status SetTableHeapRoot(TableId table_id, PageId heap_root_page_id) override;
  Result<std::vector<TableInfo>> ListTables() const override;
  Result<IndexInfo> CreateIndex(const CreateIndexRequest &request) override;
  Result<IndexInfo> GetIndex(std::string_view table_name,
                             std::string_view index_name) const override;

private:
  std::unique_ptr<HostedCatalogApi> hosted_api_;
};

} // namespace mattsql
