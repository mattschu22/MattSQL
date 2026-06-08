#include "mattsql/storage/in_memory_table_storage.hpp"

#include "mattsql/common/result_utils.hpp"
#include "mattsql/common/trace.hpp"
#include "mattsql/storage/heap/page_heap_table.hpp"

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <utility>

namespace mattsql {
namespace {

struct InMemoryTableData {
  TableId table_id = 0;
  PageId root_page_id = kInvalidPageId;
  PageHeapTable heap;
};

} // namespace

struct InMemoryTableStorageManager::Impl {
  PageId next_root_page_id = 1;
  std::unordered_map<TableId, InMemoryTableData> tables;
};

InMemoryTableStorageManager::InMemoryTableStorageManager()
    : impl_(std::make_unique<Impl>()) {}

InMemoryTableStorageManager::~InMemoryTableStorageManager() = default;

InMemoryTableStorageManager::InMemoryTableStorageManager(
    InMemoryTableStorageManager &&) noexcept = default;

InMemoryTableStorageManager &InMemoryTableStorageManager::operator=(
    InMemoryTableStorageManager &&) noexcept = default;

Result<PageId> InMemoryTableStorageManager::CreateHeap(Transaction &transaction,
                                                       const TableInfo &table) {
  ScopedTrace trace("mattsql::InMemoryTableStorageManager::CreateHeap",
                    "function.storage");
  (void)transaction;
  if (impl_->tables.contains(table.id)) {
    return error_result<PageId>(ErrorCode::AlreadyExists, "heap already exists");
  }
  if (impl_->next_root_page_id == kInvalidPageId) {
    return error_result<PageId>(ErrorCode::Internal,
                                "in-memory heap root id space exhausted");
  }

  const auto root_page_id = impl_->next_root_page_id++;
  InMemoryTableData data{table.id, root_page_id, PageHeapTable(root_page_id)};
  impl_->tables.emplace(table.id, std::move(data));
  return ok_result(root_page_id);
}

Result<std::unique_ptr<HeapTable>>
InMemoryTableStorageManager::OpenHeap(const TableStorageReference &reference) {
  ScopedTrace trace("mattsql::InMemoryTableStorageManager::OpenHeap",
                    "function.storage");
  if (reference.method != TableStorageMethod::Heap) {
    return error_result<std::unique_ptr<HeapTable>>(
        ErrorCode::NotSupported, "only heap table storage is supported");
  }

  const auto table_iter = impl_->tables.find(reference.table_id);
  if (table_iter == impl_->tables.end()) {
    return error_result<std::unique_ptr<HeapTable>>(ErrorCode::NotFound,
                                                    "heap not found");
  }
  if (reference.root_page_id != table_iter->second.root_page_id) {
    return error_result<std::unique_ptr<HeapTable>>(ErrorCode::Corruption,
                                                    "heap root page mismatch");
  }

  std::unique_ptr<HeapTable> handle = table_iter->second.heap.OpenHandle();
  return ok_result<std::unique_ptr<HeapTable>>(std::move(handle));
}

Result<std::size_t> InMemoryTableStorageManager::RecordCount(TableId table_id) const {
  ScopedTrace trace("mattsql::InMemoryTableStorageManager::RecordCount",
                    "function.storage");
  const auto table_iter = impl_->tables.find(table_id);
  if (table_iter == impl_->tables.end()) {
    return error_result<std::size_t>(ErrorCode::NotFound, "heap not found");
  }

  return ok_result(table_iter->second.heap.RecordCount());
}

} // namespace mattsql
