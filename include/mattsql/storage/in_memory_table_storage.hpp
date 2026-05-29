#pragma once

#include "mattsql/storage/table_storage.hpp"

#include <cstddef>
#include <memory>

namespace mattsql {

class InMemoryTableStorageManager final : public TableStorageManager {
public:
  InMemoryTableStorageManager();
  ~InMemoryTableStorageManager() override;

  InMemoryTableStorageManager(const InMemoryTableStorageManager &) = delete;
  InMemoryTableStorageManager &operator=(const InMemoryTableStorageManager &) = delete;

  InMemoryTableStorageManager(InMemoryTableStorageManager &&) noexcept;
  InMemoryTableStorageManager &operator=(InMemoryTableStorageManager &&) noexcept;

  /// Creates an in-memory heap root for a catalog table.
  Result<PageId> CreateHeap(Transaction &transaction, const TableInfo &table) override;

  /// Opens an in-memory heap by stable table and root-page identifiers.
  Result<std::unique_ptr<HeapTable>>
  OpenHeap(const TableStorageReference &reference) override;

  /// Returns live record count for hosted tests and diagnostics.
  [[nodiscard]] Result<std::size_t> RecordCount(TableId table_id) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace mattsql
