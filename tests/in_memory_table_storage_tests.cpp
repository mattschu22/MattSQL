#include "mattsql/common/result_utils.hpp"
#include "mattsql/storage/in_memory_table_storage.hpp"

#include "test_framework.hpp"

#include <cstddef>
#include <span>
#include <string>
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

mattsql::TableInfo make_table(mattsql::TableId table_id = 7) {
  mattsql::TableInfo table;
  table.id = table_id;
  table.name = "users";
  table.schema.columns.push_back({"id", mattsql::SqlType::Integer, false});
  return table;
}

mattsql::TableStorageReference make_reference(const mattsql::TableInfo &table,
                                              mattsql::PageId root_page_id) {
  mattsql::TableStorageReference reference;
  reference.method = mattsql::TableStorageMethod::Heap;
  reference.table_id = table.id;
  reference.table_name = table.name;
  reference.root_page_id = root_page_id;
  reference.schema = table.schema;
  return reference;
}

mattsql::ConstBufferView view_bytes(const std::vector<std::byte> &bytes) {
  return mattsql::ConstBufferView{
      std::span<const std::byte>(bytes.data(), bytes.size())};
}

} // namespace

/// Verifies hosted in-memory heap storage supports create/open/insert/read/scan.
TEST_CASE(in_memory_table_storage_round_trips_records) {
  mattsql::InMemoryTableStorageManager storage;
  TestTransaction transaction;
  const auto table = make_table();

  const auto root = storage.CreateHeap(transaction, table);
  EXPECT_TRUE(mattsql::status_ok(root.status));

  const auto reference = make_reference(table, *root.value);
  auto heap = storage.OpenHeap(reference);
  EXPECT_TRUE(mattsql::status_ok(heap.status));

  const std::vector<std::byte> bytes = {std::byte{1}, std::byte{2}, std::byte{3}};
  const auto inserted = (*heap.value)->Insert(transaction, view_bytes(bytes));
  EXPECT_TRUE(mattsql::status_ok(inserted.status));
  EXPECT_EQ(inserted.value->page_id, *root.value);
  EXPECT_EQ(inserted.value->slot_id, mattsql::SlotId{0});

  const auto count = storage.RecordCount(table.id);
  EXPECT_TRUE(mattsql::status_ok(count.status));
  EXPECT_EQ(*count.value, 1U);

  const auto read = (*heap.value)->Read(transaction, *inserted.value);
  EXPECT_TRUE(mattsql::status_ok(read.status));
  EXPECT_EQ(read.value->bytes.bytes.size(), bytes.size());
  EXPECT_TRUE(read.value->bytes.bytes[0] == bytes[0]);
  EXPECT_TRUE(read.value->bytes.bytes[1] == bytes[1]);
  EXPECT_TRUE(read.value->bytes.bytes[2] == bytes[2]);

  auto cursor = (*heap.value)->Scan(transaction);
  EXPECT_TRUE(mattsql::status_ok(cursor.status));
  const auto scanned = (*cursor.value)->Next();
  EXPECT_TRUE(mattsql::status_ok(scanned.status));
  EXPECT_EQ(scanned.value->id.slot_id, mattsql::SlotId{0});
  EXPECT_TRUE((*cursor.value)->Next().status.code == mattsql::ErrorCode::NotFound);
}

/// Verifies deletes preserve slot validity rules and scans skip tombstones.
TEST_CASE(in_memory_table_storage_deletes_records) {
  mattsql::InMemoryTableStorageManager storage;
  TestTransaction transaction;
  const auto table = make_table();
  const auto root = storage.CreateHeap(transaction, table);
  EXPECT_TRUE(mattsql::status_ok(root.status));

  auto heap = storage.OpenHeap(make_reference(table, *root.value));
  EXPECT_TRUE(mattsql::status_ok(heap.status));

  const std::vector<std::byte> bytes = {std::byte{9}};
  const auto inserted = (*heap.value)->Insert(transaction, view_bytes(bytes));
  EXPECT_TRUE(mattsql::status_ok(inserted.status));

  EXPECT_TRUE(mattsql::status_ok((*heap.value)->Delete(transaction, *inserted.value)));
  EXPECT_TRUE((*heap.value)->Read(transaction, *inserted.value).status.code ==
              mattsql::ErrorCode::NotFound);

  const auto count = storage.RecordCount(table.id);
  EXPECT_TRUE(mattsql::status_ok(count.status));
  EXPECT_EQ(*count.value, 0U);

  auto cursor = (*heap.value)->Scan(transaction);
  EXPECT_TRUE(mattsql::status_ok(cursor.status));
  EXPECT_TRUE((*cursor.value)->Next().status.code == mattsql::ErrorCode::NotFound);
}

/// Verifies hosted storage rejects invalid heap references and duplicates.
TEST_CASE(in_memory_table_storage_rejects_invalid_requests) {
  mattsql::InMemoryTableStorageManager storage;
  TestTransaction transaction;
  const auto table = make_table();

  EXPECT_TRUE(storage.OpenHeap(make_reference(table, 99)).status.code ==
              mattsql::ErrorCode::NotFound);
  EXPECT_TRUE(storage.RecordCount(table.id).status.code ==
              mattsql::ErrorCode::NotFound);

  const auto root = storage.CreateHeap(transaction, table);
  EXPECT_TRUE(mattsql::status_ok(root.status));
  EXPECT_TRUE(storage.CreateHeap(transaction, table).status.code ==
              mattsql::ErrorCode::AlreadyExists);

  auto wrong_root = make_reference(table, *root.value + 1);
  EXPECT_TRUE(storage.OpenHeap(wrong_root).status.code ==
              mattsql::ErrorCode::Corruption);

  auto bad_method = make_reference(table, *root.value);
  bad_method.method = static_cast<mattsql::TableStorageMethod>(99);
  EXPECT_TRUE(storage.OpenHeap(bad_method).status.code ==
              mattsql::ErrorCode::NotSupported);

  auto heap = storage.OpenHeap(make_reference(table, *root.value));
  EXPECT_TRUE(mattsql::status_ok(heap.status));
  mattsql::RecordId wrong_page;
  wrong_page.page_id = *root.value + 1;
  wrong_page.slot_id = 0;
  EXPECT_TRUE((*heap.value)->Delete(transaction, wrong_page).code ==
              mattsql::ErrorCode::NotFound);
}
