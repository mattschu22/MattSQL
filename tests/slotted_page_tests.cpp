#include "mattsql/common/result_utils.hpp"
#include "mattsql/storage/page/slotted_page.hpp"

#include "test_framework.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct TestPage {
  explicit TestPage(std::size_t size = 128) : bytes(size) {
    header.page_id = mattsql::PageId{42};
  }

  mattsql::PageView Mutable() {
    return mattsql::PageView{&header, mattsql::BufferView{std::span<std::byte>(bytes)}};
  }

  mattsql::ConstPageView Const() const {
    return mattsql::ConstPageView{
        &header, mattsql::ConstBufferView{std::span<const std::byte>(bytes)}};
  }

  mattsql::PageHeader header;
  std::vector<std::byte> bytes;
};

std::vector<std::byte> bytes_of(std::string_view text) {
  std::vector<std::byte> bytes;
  bytes.reserve(text.size());
  for (const auto character : text) {
    bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
  }
  return bytes;
}

mattsql::ConstBufferView view_of(const std::vector<std::byte> &bytes) {
  return mattsql::ConstBufferView{
      std::span<const std::byte>(bytes.data(), bytes.size())};
}

void expect_record_bytes(mattsql::RecordView record, std::string_view expected) {
  EXPECT_EQ(record.bytes.bytes.size(), expected.size());
  for (std::size_t index = 0; index < expected.size(); ++index) {
    EXPECT_TRUE(record.bytes.bytes[index] ==
                static_cast<std::byte>(static_cast<unsigned char>(expected[index])));
  }
}

} // namespace

/// Verifies initialization sets page metadata and clears usable bytes.
TEST_CASE(slotted_page_initializes_empty_pages) {
  mattsql::DefaultSlottedPage slotted_page;
  TestPage page;
  std::fill(page.bytes.begin(), page.bytes.end(), std::byte{0x7F});

  const auto status = slotted_page.Initialize(page.Mutable(), mattsql::PageKind::Heap);

  EXPECT_TRUE(mattsql::status_ok(status));
  EXPECT_EQ(page.header.page_id, mattsql::PageId{42});
  EXPECT_TRUE(page.header.kind == mattsql::PageKind::Heap);
  EXPECT_EQ(page.header.page_lsn, mattsql::kInvalidLsn);
  EXPECT_EQ(page.header.flags, std::uint16_t{0});
  EXPECT_EQ(page.header.lower, std::uint16_t{0});
  EXPECT_EQ(page.header.upper, static_cast<std::uint16_t>(page.bytes.size()));
  EXPECT_EQ(slotted_page.FreeSpace(page.Const()), page.bytes.size());
  for (const auto byte : page.bytes) {
    EXPECT_TRUE(byte == std::byte{0});
  }
}

/// Verifies inserted records can be read back by stable slot identifiers.
TEST_CASE(slotted_page_inserts_and_reads_records) {
  mattsql::DefaultSlottedPage slotted_page;
  TestPage page;
  EXPECT_TRUE(mattsql::status_ok(
      slotted_page.Initialize(page.Mutable(), mattsql::PageKind::Heap)));

  const auto first_bytes = bytes_of("abc");
  const auto second_bytes = bytes_of("longer");

  const auto first = slotted_page.Insert(page.Mutable(), view_of(first_bytes));
  const auto second = slotted_page.Insert(page.Mutable(), view_of(second_bytes));

  EXPECT_TRUE(mattsql::status_ok(first.status));
  EXPECT_TRUE(mattsql::status_ok(second.status));
  EXPECT_EQ(*first.value, mattsql::SlotId{0});
  EXPECT_EQ(*second.value, mattsql::SlotId{1});
  EXPECT_EQ(page.header.lower, std::uint16_t{12});
  EXPECT_EQ(page.header.upper,
            static_cast<std::uint16_t>(page.bytes.size() - first_bytes.size() -
                                       second_bytes.size()));
  EXPECT_EQ(slotted_page.FreeSpace(page.Const()),
            page.bytes.size() - first_bytes.size() - second_bytes.size() - 12U);

  const auto first_read = slotted_page.Read(page.Const(), *first.value);
  const auto second_read = slotted_page.Read(page.Const(), *second.value);

  EXPECT_TRUE(mattsql::status_ok(first_read.status));
  EXPECT_TRUE(mattsql::status_ok(second_read.status));
  EXPECT_EQ(first_read.value->id.page_id, mattsql::PageId{42});
  EXPECT_EQ(first_read.value->id.slot_id, mattsql::SlotId{0});
  expect_record_bytes(*first_read.value, "abc");
  expect_record_bytes(*second_read.value, "longer");
}

/// Verifies deleted slots are tombstoned and can be reused by later inserts.
TEST_CASE(slotted_page_deletes_and_reuses_slots) {
  mattsql::DefaultSlottedPage slotted_page;
  TestPage page;
  EXPECT_TRUE(mattsql::status_ok(
      slotted_page.Initialize(page.Mutable(), mattsql::PageKind::Heap)));

  const auto first_bytes = bytes_of("a");
  const auto second_bytes = bytes_of("b");
  const auto third_bytes = bytes_of("cc");

  const auto first = slotted_page.Insert(page.Mutable(), view_of(first_bytes));
  const auto second = slotted_page.Insert(page.Mutable(), view_of(second_bytes));
  EXPECT_TRUE(mattsql::status_ok(first.status));
  EXPECT_TRUE(mattsql::status_ok(second.status));
  const auto lower_after_two_inserts = page.header.lower;

  EXPECT_TRUE(mattsql::status_ok(slotted_page.Delete(page.Mutable(), *first.value)));
  EXPECT_TRUE(slotted_page.Read(page.Const(), *first.value).status.code ==
              mattsql::ErrorCode::NotFound);

  const auto third = slotted_page.Insert(page.Mutable(), view_of(third_bytes));
  EXPECT_TRUE(mattsql::status_ok(third.status));
  EXPECT_EQ(*third.value, mattsql::SlotId{0});
  EXPECT_EQ(page.header.lower, lower_after_two_inserts);

  const auto reused = slotted_page.Read(page.Const(), *third.value);
  const auto still_active = slotted_page.Read(page.Const(), *second.value);
  EXPECT_TRUE(mattsql::status_ok(reused.status));
  EXPECT_TRUE(mattsql::status_ok(still_active.status));
  expect_record_bytes(*reused.value, "cc");
  expect_record_bytes(*still_active.value, "b");
}

/// Verifies invalid operations fail with status codes rather than corrupting data.
TEST_CASE(slotted_page_rejects_invalid_operations) {
  mattsql::DefaultSlottedPage slotted_page;
  TestPage page(24);

  EXPECT_TRUE(slotted_page.Insert(page.Mutable(), view_of(bytes_of("x"))).status.code ==
              mattsql::ErrorCode::Corruption);
  EXPECT_TRUE(slotted_page.Initialize(page.Mutable(), mattsql::PageKind::Free).code ==
              mattsql::ErrorCode::InvalidArgument);
  EXPECT_TRUE(mattsql::status_ok(
      slotted_page.Initialize(page.Mutable(), mattsql::PageKind::Heap)));

  const auto too_large = bytes_of("this record cannot fit");
  EXPECT_TRUE(slotted_page.Insert(page.Mutable(), view_of(too_large)).status.code ==
              mattsql::ErrorCode::InvalidArgument);
  EXPECT_TRUE(slotted_page.Read(page.Const(), mattsql::SlotId{3}).status.code ==
              mattsql::ErrorCode::NotFound);
  EXPECT_TRUE(slotted_page.Delete(page.Mutable(), mattsql::SlotId{3}).code ==
              mattsql::ErrorCode::NotFound);
}

/// Verifies malformed page metadata and slot entries are detected.
TEST_CASE(slotted_page_detects_corruption) {
  mattsql::DefaultSlottedPage slotted_page;
  TestPage page;
  EXPECT_TRUE(mattsql::status_ok(
      slotted_page.Initialize(page.Mutable(), mattsql::PageKind::Heap)));

  page.header.lower = 5;
  EXPECT_EQ(slotted_page.FreeSpace(page.Const()), 0U);
  EXPECT_TRUE(slotted_page.Insert(page.Mutable(), view_of(bytes_of("x"))).status.code ==
              mattsql::ErrorCode::Corruption);

  EXPECT_TRUE(mattsql::status_ok(
      slotted_page.Initialize(page.Mutable(), mattsql::PageKind::Heap)));
  const auto inserted = slotted_page.Insert(page.Mutable(), view_of(bytes_of("abc")));
  EXPECT_TRUE(mattsql::status_ok(inserted.status));

  // Corrupt the first slot offset so it points before the record area.
  page.bytes[0] = std::byte{0};
  page.bytes[1] = std::byte{0};
  EXPECT_TRUE(slotted_page.Read(page.Const(), *inserted.value).status.code ==
              mattsql::ErrorCode::Corruption);
  EXPECT_TRUE(slotted_page.Delete(page.Mutable(), *inserted.value).code ==
              mattsql::ErrorCode::Corruption);
}
