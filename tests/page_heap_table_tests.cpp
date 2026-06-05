#include "mattsql/common/result_utils.hpp"
#include "mattsql/storage/heap/page_heap_table.hpp"

#include "test_framework.hpp"

#include <cstddef>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace {

class TestTransaction final : public mattsql::Transaction {
public:
  mattsql::TransactionId Id() const override { return 1; }
  mattsql::TransactionMode Mode() const override {
    return mattsql::TransactionMode::ReadWrite;
  }
  mattsql::TransactionState State() const override {
    return mattsql::TransactionState::Active;
  }
  mattsql::LogSequenceNumber BeginLsn() const override {
    return mattsql::LogSequenceNumber{0};
  }
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

/// Verifies a page-backed heap can insert and read records on one page.
TEST_CASE(page_heap_table_inserts_and_reads_records) {
  mattsql::PageHeapTable heap(mattsql::PageId{100}, 64);
  TestTransaction transaction;

  const auto first_bytes = bytes_of("alpha");
  const auto second_bytes = bytes_of("beta");

  const auto first = heap.Insert(transaction, view_of(first_bytes));
  const auto second = heap.Insert(transaction, view_of(second_bytes));

  EXPECT_TRUE(mattsql::status_ok(first.status));
  EXPECT_TRUE(mattsql::status_ok(second.status));
  EXPECT_EQ(first.value->page_id, mattsql::PageId{100});
  EXPECT_EQ(first.value->slot_id, mattsql::SlotId{0});
  EXPECT_EQ(second.value->page_id, mattsql::PageId{100});
  EXPECT_EQ(second.value->slot_id, mattsql::SlotId{1});
  EXPECT_EQ(heap.PageCount(), 1U);
  EXPECT_EQ(heap.RecordCount(), 2U);

  const auto first_read = heap.Read(transaction, *first.value);
  const auto second_read = heap.Read(transaction, *second.value);
  EXPECT_TRUE(mattsql::status_ok(first_read.status));
  EXPECT_TRUE(mattsql::status_ok(second_read.status));
  expect_record_bytes(*first_read.value, "alpha");
  expect_record_bytes(*second_read.value, "beta");
}

/// Verifies inserts allocate additional heap pages when existing pages fill.
TEST_CASE(page_heap_table_allocates_additional_pages) {
  mattsql::PageHeapTable heap(mattsql::PageId{200}, 24);
  TestTransaction transaction;

  const auto record = bytes_of("abcdefghij");
  const auto first = heap.Insert(transaction, view_of(record));
  const auto second = heap.Insert(transaction, view_of(record));

  EXPECT_TRUE(mattsql::status_ok(first.status));
  EXPECT_TRUE(mattsql::status_ok(second.status));
  EXPECT_EQ(first.value->page_id, mattsql::PageId{200});
  EXPECT_EQ(second.value->page_id, mattsql::PageId{201});
  EXPECT_EQ(heap.PageCount(), 2U);
  EXPECT_EQ(heap.RecordCount(), 2U);
}

/// Verifies scans walk records in page and slot order and skip deleted slots.
TEST_CASE(page_heap_table_scans_and_skips_deleted_records) {
  mattsql::PageHeapTable heap(mattsql::PageId{300}, 32);
  TestTransaction transaction;

  const auto first_bytes = bytes_of("one");
  const auto second_bytes = bytes_of("two");
  const auto third_bytes = bytes_of("three");

  const auto first = heap.Insert(transaction, view_of(first_bytes));
  const auto second = heap.Insert(transaction, view_of(second_bytes));
  const auto third = heap.Insert(transaction, view_of(third_bytes));
  EXPECT_TRUE(mattsql::status_ok(first.status));
  EXPECT_TRUE(mattsql::status_ok(second.status));
  EXPECT_TRUE(mattsql::status_ok(third.status));
  EXPECT_TRUE(mattsql::status_ok(heap.Delete(transaction, *second.value)));
  EXPECT_EQ(heap.RecordCount(), 2U);

  auto cursor = heap.Scan(transaction);
  EXPECT_TRUE(mattsql::status_ok(cursor.status));

  const auto scanned_first = (*cursor.value)->Next();
  const auto scanned_second = (*cursor.value)->Next();
  EXPECT_TRUE(mattsql::status_ok(scanned_first.status));
  EXPECT_TRUE(mattsql::status_ok(scanned_second.status));
  expect_record_bytes(*scanned_first.value, "one");
  expect_record_bytes(*scanned_second.value, "three");
  EXPECT_TRUE((*cursor.value)->Next().status.code == mattsql::ErrorCode::NotFound);
}

/// Verifies shared handles opened over the same heap observe the same pages.
TEST_CASE(page_heap_table_handles_share_heap_state) {
  mattsql::PageHeapTable heap(mattsql::PageId{400}, 64);
  TestTransaction transaction;

  auto handle = heap.OpenHandle();
  const auto bytes = bytes_of("shared");
  const auto inserted = handle->Insert(transaction, view_of(bytes));
  EXPECT_TRUE(mattsql::status_ok(inserted.status));
  EXPECT_EQ(heap.RecordCount(), 1U);

  const auto read = heap.Read(transaction, *inserted.value);
  EXPECT_TRUE(mattsql::status_ok(read.status));
  expect_record_bytes(*read.value, "shared");
}

/// Verifies invalid page ids, slots, and impossible records are rejected.
TEST_CASE(page_heap_table_rejects_invalid_requests) {
  mattsql::PageHeapTable heap(mattsql::PageId{500}, 16);
  TestTransaction transaction;

  mattsql::RecordId missing_page;
  missing_page.page_id = mattsql::PageId{999};
  missing_page.slot_id = 0;
  EXPECT_TRUE(heap.Read(transaction, missing_page).status.code ==
              mattsql::ErrorCode::NotFound);
  EXPECT_TRUE(heap.Delete(transaction, missing_page).code ==
              mattsql::ErrorCode::NotFound);

  const auto too_large = bytes_of("this record cannot fit");
  EXPECT_TRUE(heap.Insert(transaction, view_of(too_large)).status.code ==
              mattsql::ErrorCode::InvalidArgument);
  EXPECT_EQ(heap.PageCount(), 0U);
  EXPECT_EQ(heap.RecordCount(), 0U);

  const auto bytes = bytes_of("x");
  const auto inserted = heap.Insert(transaction, view_of(bytes));
  EXPECT_TRUE(mattsql::status_ok(inserted.status));
  mattsql::RecordId missing_slot = *inserted.value;
  missing_slot.slot_id = mattsql::SlotId{5};
  EXPECT_TRUE(heap.Read(transaction, missing_slot).status.code ==
              mattsql::ErrorCode::NotFound);
  EXPECT_TRUE(heap.Delete(transaction, missing_slot).code ==
              mattsql::ErrorCode::NotFound);
}

/// Verifies heaps reject page id allocation once the valid id range is exhausted.
TEST_CASE(page_heap_table_rejects_page_id_exhaustion) {
  mattsql::PageHeapTable heap(std::numeric_limits<mattsql::PageId>::max() - 1U, 24);
  TestTransaction transaction;
  const auto record = bytes_of("abcdefghij");

  const auto first = heap.Insert(transaction, view_of(record));
  EXPECT_TRUE(mattsql::status_ok(first.status));
  EXPECT_EQ(first.value->page_id,
            std::numeric_limits<mattsql::PageId>::max() - 1U);

  const auto second = heap.Insert(transaction, view_of(record));
  EXPECT_TRUE(second.status.code == mattsql::ErrorCode::Internal);
  EXPECT_EQ(heap.PageCount(), 1U);
  EXPECT_EQ(heap.RecordCount(), 1U);
}
