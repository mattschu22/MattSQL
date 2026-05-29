#include "mattsql/storage/page/slotted_page.hpp"

#include "mattsql/common/result_utils.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace mattsql {
namespace {

constexpr std::size_t kSlotEntrySize = 6;
constexpr std::uint16_t kSlotDeleted = 1;

struct SlotEntry {
  std::uint16_t offset = 0;
  std::uint16_t size = 0;
  std::uint16_t flags = 0;
};

[[nodiscard]] std::uint16_t read_u16(std::span<const std::byte> bytes,
                                     std::size_t offset) {
  const auto low = std::to_integer<std::uint16_t>(bytes[offset]);
  const auto high = std::to_integer<std::uint16_t>(bytes[offset + 1]);
  return static_cast<std::uint16_t>(low | static_cast<std::uint16_t>(high << 8U));
}

void write_u16(std::span<std::byte> bytes, std::size_t offset, std::uint16_t value) {
  bytes[offset] = static_cast<std::byte>(value & 0x00FFU);
  bytes[offset + 1] = static_cast<std::byte>((value >> 8U) & 0x00FFU);
}

[[nodiscard]] SlotEntry read_slot(std::span<const std::byte> bytes, SlotId slot_id) {
  const auto offset = static_cast<std::size_t>(slot_id) * kSlotEntrySize;
  return SlotEntry{read_u16(bytes, offset), read_u16(bytes, offset + 2),
                   read_u16(bytes, offset + 4)};
}

void write_slot(std::span<std::byte> bytes, SlotId slot_id, SlotEntry entry) {
  const auto offset = static_cast<std::size_t>(slot_id) * kSlotEntrySize;
  write_u16(bytes, offset, entry.offset);
  write_u16(bytes, offset + 2, entry.size);
  write_u16(bytes, offset + 4, entry.flags);
}

[[nodiscard]] bool is_deleted(SlotEntry entry) {
  return (entry.flags & kSlotDeleted) != 0;
}

[[nodiscard]] Status validate_header(const PageHeader *header,
                                     std::span<const std::byte> bytes) {
  if (header == nullptr) {
    return error_status(ErrorCode::InvalidArgument, "page header is required");
  }
  if (bytes.empty()) {
    return error_status(ErrorCode::InvalidArgument, "page bytes are required");
  }
  if (bytes.size() > std::numeric_limits<std::uint16_t>::max()) {
    return error_status(ErrorCode::InvalidArgument,
                        "slotted page is larger than 16-bit offsets support");
  }
  if (header->kind == PageKind::Free) {
    return error_status(ErrorCode::Corruption, "slotted page is not initialized");
  }
  if (header->lower > header->upper || header->upper > bytes.size()) {
    return error_status(ErrorCode::Corruption,
                        "slotted page free-space pointers are invalid");
  }
  if (header->lower % kSlotEntrySize != 0) {
    return error_status(ErrorCode::Corruption,
                        "slotted page lower pointer is not slot aligned");
  }

  return ok_status();
}

[[nodiscard]] Status validate_page(ConstPageView page) {
  return validate_header(page.header, page.bytes.bytes);
}

[[nodiscard]] Status validate_page(PageView page) {
  return validate_header(page.header, page.bytes.bytes);
}

[[nodiscard]] std::size_t slot_count(const PageHeader &header) {
  return header.lower / kSlotEntrySize;
}

[[nodiscard]] Status validate_slot_bounds(ConstPageView page, SlotEntry entry) {
  if (entry.offset > page.bytes.bytes.size() ||
      entry.size > page.bytes.bytes.size() - entry.offset) {
    return error_status(ErrorCode::Corruption, "slot points outside the page payload");
  }
  if (entry.offset < page.header->upper) {
    return error_status(ErrorCode::Corruption, "slot points below the record area");
  }

  return ok_status();
}

[[nodiscard]] std::optional<SlotId> find_deleted_slot(ConstPageView page) {
  const auto count = slot_count(*page.header);
  for (std::size_t index = 0; index < count; ++index) {
    const auto slot_id = static_cast<SlotId>(index);
    if (is_deleted(read_slot(page.bytes.bytes, slot_id))) {
      return slot_id;
    }
  }

  return std::nullopt;
}

[[nodiscard]] ConstPageView as_const(PageView page) {
  return ConstPageView{page.header, ConstBufferView{page.bytes.bytes}};
}

} // namespace

Status DefaultSlottedPage::Initialize(PageView page, PageKind kind) {
  if (page.header == nullptr) {
    return error_status(ErrorCode::InvalidArgument, "page header is required");
  }
  if (page.bytes.bytes.empty()) {
    return error_status(ErrorCode::InvalidArgument, "page bytes are required");
  }
  if (page.bytes.bytes.size() > std::numeric_limits<std::uint16_t>::max()) {
    return error_status(ErrorCode::InvalidArgument,
                        "slotted page is larger than 16-bit offsets support");
  }
  if (kind == PageKind::Free) {
    return error_status(ErrorCode::InvalidArgument, "slotted page kind cannot be Free");
  }

  page.header->kind = kind;
  page.header->page_lsn = kInvalidLsn;
  page.header->flags = 0;
  page.header->lower = 0;
  page.header->upper = static_cast<std::uint16_t>(page.bytes.bytes.size());
  std::fill(page.bytes.bytes.begin(), page.bytes.bytes.end(), std::byte{0});
  return ok_status();
}

Result<SlotId> DefaultSlottedPage::Insert(PageView page, ConstBufferView record) {
  const auto page_status = validate_page(page);
  if (!status_ok(page_status)) {
    return error_result<SlotId>(page_status);
  }
  if (record.bytes.size() > std::numeric_limits<std::uint16_t>::max()) {
    return error_result<SlotId>(ErrorCode::InvalidArgument,
                                "record is larger than 16-bit slots support");
  }

  const auto deleted_slot = find_deleted_slot(as_const(page));
  const auto slot_space = deleted_slot.has_value() ? 0U : kSlotEntrySize;
  const auto required_space = record.bytes.size() + slot_space;
  if (required_space > static_cast<std::size_t>(page.header->upper) -
                           static_cast<std::size_t>(page.header->lower)) {
    return error_result<SlotId>(ErrorCode::InvalidArgument,
                                "not enough free space in slotted page");
  }

  const auto record_offset =
      static_cast<std::uint16_t>(page.header->upper - record.bytes.size());
  std::copy(record.bytes.begin(), record.bytes.end(),
            page.bytes.bytes.begin() + record_offset);

  SlotId slot_id = 0;
  if (deleted_slot.has_value()) {
    slot_id = *deleted_slot;
  } else {
    slot_id = static_cast<SlotId>(slot_count(*page.header));
    page.header->lower =
        static_cast<std::uint16_t>(page.header->lower + kSlotEntrySize);
  }

  write_slot(
      page.bytes.bytes, slot_id,
      SlotEntry{record_offset, static_cast<std::uint16_t>(record.bytes.size()), 0});
  page.header->upper = record_offset;
  return ok_result(slot_id);
}

Result<RecordView> DefaultSlottedPage::Read(ConstPageView page, SlotId slot_id) const {
  const auto page_status = validate_page(page);
  if (!status_ok(page_status)) {
    return error_result<RecordView>(page_status);
  }
  if (slot_id >= slot_count(*page.header)) {
    return error_result<RecordView>(ErrorCode::NotFound, "slot not found");
  }

  const auto entry = read_slot(page.bytes.bytes, slot_id);
  if (is_deleted(entry)) {
    return error_result<RecordView>(ErrorCode::NotFound, "slot is deleted");
  }

  const auto slot_status = validate_slot_bounds(page, entry);
  if (!status_ok(slot_status)) {
    return error_result<RecordView>(slot_status);
  }

  RecordView view;
  view.id.page_id = page.header->page_id;
  view.id.slot_id = slot_id;
  view.bytes = ConstBufferView{
      std::span<const std::byte>(page.bytes.bytes.data() + entry.offset, entry.size)};
  return ok_result(view);
}

Status DefaultSlottedPage::Delete(PageView page, SlotId slot_id) {
  const auto page_status = validate_page(page);
  if (!status_ok(page_status)) {
    return page_status;
  }
  if (slot_id >= slot_count(*page.header)) {
    return error_status(ErrorCode::NotFound, "slot not found");
  }

  auto entry = read_slot(page.bytes.bytes, slot_id);
  if (is_deleted(entry)) {
    return error_status(ErrorCode::NotFound, "slot is deleted");
  }

  const auto slot_status = validate_slot_bounds(as_const(page), entry);
  if (!status_ok(slot_status)) {
    return slot_status;
  }

  entry.flags |= kSlotDeleted;
  write_slot(page.bytes.bytes, slot_id, entry);
  return ok_status();
}

std::size_t DefaultSlottedPage::FreeSpace(ConstPageView page) const {
  const auto page_status = validate_page(page);
  if (!status_ok(page_status)) {
    return 0;
  }

  return static_cast<std::size_t>(page.header->upper) -
         static_cast<std::size_t>(page.header->lower);
}

std::size_t DefaultSlottedPage::SlotCount(ConstPageView page) const {
  const auto page_status = validate_page(page);
  if (!status_ok(page_status)) {
    return 0;
  }

  return slot_count(*page.header);
}

} // namespace mattsql
