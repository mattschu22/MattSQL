#pragma once

#include "mattsql/storage/heap/heap_table.hpp"

#include <cstddef>
#include <memory>

namespace mattsql {

class PageHeapTable final : public HeapTable {
public:
  /// Creates an empty page-backed heap rooted at the supplied page id.
  explicit PageHeapTable(PageId root_page_id, std::size_t page_size = kDefaultPageSize);

  PageHeapTable(const PageHeapTable &) = delete;
  PageHeapTable &operator=(const PageHeapTable &) = delete;

  PageHeapTable(PageHeapTable &&) noexcept;
  PageHeapTable &operator=(PageHeapTable &&) noexcept;
  ~PageHeapTable() override;

  /// Inserts a serialized row into the first page with enough contiguous space.
  Result<RecordId> Insert(Transaction &transaction, ConstBufferView record) override;

  /// Reads a serialized row by page and slot identifier.
  Result<RecordView> Read(Transaction &transaction, RecordId record_id) override;

  /// Deletes a row by page and slot identifier.
  Status Delete(Transaction &transaction, RecordId record_id) override;

  /// Opens a sequential scan over active records in page/slot order.
  Result<std::unique_ptr<HeapCursor>> Scan(Transaction &transaction) override;

  /// Creates another table handle over the same page-backed heap state.
  [[nodiscard]] std::unique_ptr<PageHeapTable> OpenHandle() const;

  /// Returns the heap root page id.
  [[nodiscard]] PageId RootPageId() const;

  /// Returns the number of pages currently allocated to this heap.
  [[nodiscard]] std::size_t PageCount() const;

  /// Returns active record count, excluding deleted slots.
  [[nodiscard]] std::size_t RecordCount() const;

private:
  class Cursor;
  struct Impl;

  explicit PageHeapTable(std::shared_ptr<Impl> impl);

  std::shared_ptr<Impl> impl_;
};

} // namespace mattsql
