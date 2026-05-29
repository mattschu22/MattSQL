#include "mattsql/storage/heap/page_heap_table.hpp"

#include "mattsql/common/result_utils.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace mattsql {
namespace {

struct HeapPageFrame {
  PageHeader header;
  std::vector<std::byte> bytes;

  explicit HeapPageFrame(PageId page_id, std::size_t page_size) : bytes(page_size) {
    header.page_id = page_id;
  }

  [[nodiscard]] PageView Mutable() {
    return PageView{&header, BufferView{std::span<std::byte>(bytes)}};
  }

  [[nodiscard]] ConstPageView Const() const {
    return ConstPageView{&header, ConstBufferView{std::span<const std::byte>(bytes)}};
  }
};

} // namespace

struct PageHeapTable::Impl {
  explicit Impl(PageId root_page_id, std::size_t page_size)
      : root_page_id(root_page_id), page_size(page_size) {}

  [[nodiscard]] PageId next_page_id() const {
    return root_page_id + static_cast<PageId>(pages.size());
  }

  Result<HeapPageFrame *> AllocatePage() {
    if (root_page_id == kInvalidPageId) {
      return error_result<HeapPageFrame *>(ErrorCode::InvalidArgument,
                                           "heap root page id is invalid");
    }
    if (page_size == 0) {
      return error_result<HeapPageFrame *>(ErrorCode::InvalidArgument,
                                           "heap page size must be positive");
    }

    pages.emplace_back(next_page_id(), page_size);
    auto &page = pages.back();
    const auto status = slotted_page.Initialize(page.Mutable(), PageKind::Heap);
    if (!status_ok(status)) {
      pages.pop_back();
      return error_result<HeapPageFrame *>(status);
    }

    return ok_result(&page);
  }

  [[nodiscard]] HeapPageFrame *FindPage(PageId page_id) {
    for (auto &page : pages) {
      if (page.header.page_id == page_id) {
        return &page;
      }
    }
    return nullptr;
  }

  [[nodiscard]] const HeapPageFrame *FindPage(PageId page_id) const {
    for (const auto &page : pages) {
      if (page.header.page_id == page_id) {
        return &page;
      }
    }
    return nullptr;
  }

  PageId root_page_id = kInvalidPageId;
  std::size_t page_size = kDefaultPageSize;
  std::size_t active_records = 0;
  DefaultSlottedPage slotted_page;
  std::vector<HeapPageFrame> pages;
};

class PageHeapTable::Cursor final : public HeapCursor {
public:
  explicit Cursor(std::shared_ptr<PageHeapTable::Impl> heap) : heap_(std::move(heap)) {}

  Result<RecordView> Next() override {
    while (page_index_ < heap_->pages.size()) {
      const auto &page = heap_->pages[page_index_];
      const auto slot_count = heap_->slotted_page.SlotCount(page.Const());

      while (slot_id_ < slot_count) {
        const auto current_slot = static_cast<SlotId>(slot_id_++);
        auto record = heap_->slotted_page.Read(page.Const(), current_slot);
        if (status_ok(record.status)) {
          return record;
        }
        if (record.status.code == ErrorCode::NotFound) {
          continue;
        }
        return record;
      }

      ++page_index_;
      slot_id_ = 0;
    }

    return error_result<RecordView>(ErrorCode::NotFound, "end of heap scan");
  }

private:
  std::shared_ptr<PageHeapTable::Impl> heap_;
  std::size_t page_index_ = 0;
  std::size_t slot_id_ = 0;
};

PageHeapTable::PageHeapTable(PageId root_page_id, std::size_t page_size)
    : impl_(std::make_shared<Impl>(root_page_id, page_size)) {}

PageHeapTable::PageHeapTable(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

PageHeapTable::PageHeapTable(PageHeapTable &&) noexcept = default;

PageHeapTable &PageHeapTable::operator=(PageHeapTable &&) noexcept = default;

PageHeapTable::~PageHeapTable() = default;

Result<RecordId> PageHeapTable::Insert(Transaction &transaction,
                                       ConstBufferView record) {
  (void)transaction;
  for (auto &page : impl_->pages) {
    auto slot = impl_->slotted_page.Insert(page.Mutable(), record);
    if (status_ok(slot.status)) {
      ++impl_->active_records;
      return ok_result(RecordId{page.header.page_id, *slot.value});
    }
    if (slot.status.code != ErrorCode::InvalidArgument) {
      return error_result<RecordId>(std::move(slot.status));
    }
  }

  auto page = impl_->AllocatePage();
  if (!status_ok(page.status)) {
    return error_result<RecordId>(std::move(page.status));
  }

  auto slot = impl_->slotted_page.Insert((*page.value)->Mutable(), record);
  if (!status_ok(slot.status)) {
    return error_result<RecordId>(std::move(slot.status));
  }

  ++impl_->active_records;
  return ok_result(RecordId{(*page.value)->header.page_id, *slot.value});
}

Result<RecordView> PageHeapTable::Read(Transaction &transaction, RecordId record_id) {
  (void)transaction;
  const auto *page = impl_->FindPage(record_id.page_id);
  if (page == nullptr) {
    return error_result<RecordView>(ErrorCode::NotFound, "heap page not found");
  }

  return impl_->slotted_page.Read(page->Const(), record_id.slot_id);
}

Status PageHeapTable::Delete(Transaction &transaction, RecordId record_id) {
  (void)transaction;
  auto *page = impl_->FindPage(record_id.page_id);
  if (page == nullptr) {
    return error_status(ErrorCode::NotFound, "heap page not found");
  }

  const auto status = impl_->slotted_page.Delete(page->Mutable(), record_id.slot_id);
  if (!status_ok(status)) {
    return status;
  }

  --impl_->active_records;
  return ok_status();
}

Result<std::unique_ptr<HeapCursor>> PageHeapTable::Scan(Transaction &transaction) {
  (void)transaction;
  return ok_result<std::unique_ptr<HeapCursor>>(std::make_unique<Cursor>(impl_));
}

std::unique_ptr<PageHeapTable> PageHeapTable::OpenHandle() const {
  return std::unique_ptr<PageHeapTable>(new PageHeapTable(impl_));
}

PageId PageHeapTable::RootPageId() const { return impl_->root_page_id; }

std::size_t PageHeapTable::PageCount() const { return impl_->pages.size(); }

std::size_t PageHeapTable::RecordCount() const { return impl_->active_records; }

} // namespace mattsql
