#pragma once

#include "mattsql/common/status.hpp"
#include "mattsql/common/types.hpp"
#include "mattsql/storage/page/page.hpp"

#include <cstddef>
#include <cstdint>

namespace mattsql {

using SlotId = std::uint16_t;

struct RecordId {
  PageId page_id = kInvalidPageId;
  SlotId slot_id = 0;
};

struct RecordView {
  RecordId id;
  ConstBufferView bytes;
};

class SlottedPage {
public:
  /// Destroys a slotted page adapter through the interface pointer.
  virtual ~SlottedPage() = default;

  /// Initializes an empty page with slotted-page metadata.
  virtual Status Initialize(PageView page, PageKind kind) = 0;

  /// Inserts a record and returns its slot identifier.
  virtual Result<SlotId> Insert(PageView page, ConstBufferView record) = 0;

  /// Reads a record by slot identifier.
  virtual Result<RecordView> Read(ConstPageView page, SlotId slot_id) const = 0;

  /// Marks a slot as deleted.
  virtual Status Delete(PageView page, SlotId slot_id) = 0;

  /// Returns the free space remaining in the page.
  virtual std::size_t FreeSpace(ConstPageView page) const = 0;
};

class DefaultSlottedPage final : public SlottedPage {
public:
  /// Initializes an empty page with a compact slot directory.
  Status Initialize(PageView page, PageKind kind) override;

  /// Inserts record bytes into the page's contiguous free space.
  Result<SlotId> Insert(PageView page, ConstBufferView record) override;

  /// Reads an active record by stable slot identifier.
  Result<RecordView> Read(ConstPageView page, SlotId slot_id) const override;

  /// Marks a slot as deleted without moving other records.
  Status Delete(PageView page, SlotId slot_id) override;

  /// Returns the current contiguous free space between slots and records.
  std::size_t FreeSpace(ConstPageView page) const override;

  /// Returns the number of slots recorded in the page directory.
  [[nodiscard]] std::size_t SlotCount(ConstPageView page) const;
};

} // namespace mattsql
