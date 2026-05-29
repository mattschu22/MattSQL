#include "mattsql/binder/default_binder.hpp"
#include "mattsql/catalog/in_memory_catalog.hpp"
#include "mattsql/common/result_utils.hpp"
#include "mattsql/execution/default_executor.hpp"
#include "mattsql/lexer/lexer.hpp"
#include "mattsql/optimizer/default_optimizer.hpp"
#include "mattsql/parser/parser.hpp"
#include "mattsql/planner/default_logical_planner.hpp"
#include "mattsql/planner/default_physical_planner.hpp"

#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace {

class MockTransaction final : public mattsql::Transaction {
public:
  explicit MockTransaction(
      mattsql::TransactionMode mode = mattsql::TransactionMode::ReadWrite)
      : mode_(mode) {}

  mattsql::TransactionId Id() const override { return 1; }
  mattsql::TransactionMode Mode() const override { return mode_; }
  mattsql::TransactionState State() const override { return state_; }
  mattsql::LogSequenceNumber BeginLsn() const override {
    return mattsql::LogSequenceNumber{10};
  }

  void SetState(mattsql::TransactionState state) { state_ = state; }

private:
  mattsql::TransactionMode mode_;
  mattsql::TransactionState state_ = mattsql::TransactionState::Active;
};

struct MockTableData {
  mattsql::TableId table_id = 0;
  mattsql::PageId root_page_id = mattsql::kInvalidPageId;
  std::vector<std::vector<std::byte>> records;
};

class MockHeapCursor final : public mattsql::HeapCursor {
public:
  explicit MockHeapCursor(const MockTableData &data) : data_(data) {}

  mattsql::Result<mattsql::RecordView> Next() override {
    if (index_ >= data_.records.size()) {
      return mattsql::error_result<mattsql::RecordView>(mattsql::ErrorCode::NotFound,
                                                        "end of heap scan");
    }

    const auto &record = data_.records[index_];
    mattsql::RecordView view;
    view.id.page_id = data_.root_page_id;
    view.id.slot_id = static_cast<mattsql::SlotId>(index_);
    view.bytes = mattsql::ConstBufferView{
        std::span<const std::byte>(record.data(), record.size())};
    ++index_;
    return mattsql::ok_result(view);
  }

private:
  const MockTableData &data_;
  std::size_t index_ = 0;
};

class MockHeapTable final : public mattsql::HeapTable {
public:
  explicit MockHeapTable(MockTableData &data) : data_(data) {}

  mattsql::Result<mattsql::RecordId> Insert(mattsql::Transaction &transaction,
                                            mattsql::ConstBufferView record) override {
    (void)transaction;
    std::vector<std::byte> bytes(record.bytes.begin(), record.bytes.end());
    data_.records.push_back(std::move(bytes));

    mattsql::RecordId id;
    id.page_id = data_.root_page_id;
    id.slot_id = static_cast<mattsql::SlotId>(data_.records.size() - 1);
    return mattsql::ok_result(id);
  }

  mattsql::Result<mattsql::RecordView> Read(mattsql::Transaction &transaction,
                                            mattsql::RecordId record_id) override {
    (void)transaction;
    if (record_id.slot_id >= data_.records.size()) {
      return mattsql::error_result<mattsql::RecordView>(mattsql::ErrorCode::NotFound,
                                                        "record not found");
    }

    const auto &record = data_.records[record_id.slot_id];
    mattsql::RecordView view;
    view.id = record_id;
    view.bytes = mattsql::ConstBufferView{
        std::span<const std::byte>(record.data(), record.size())};
    return mattsql::ok_result(view);
  }

  mattsql::Status Delete(mattsql::Transaction &transaction,
                         mattsql::RecordId record_id) override {
    (void)transaction;
    (void)record_id;
    return mattsql::error_status(mattsql::ErrorCode::NotSupported,
                                 "delete is not needed by executor tests");
  }

  mattsql::Result<std::unique_ptr<mattsql::HeapCursor>>
  Scan(mattsql::Transaction &transaction) override {
    (void)transaction;
    return mattsql::ok_result<std::unique_ptr<mattsql::HeapCursor>>(
        std::make_unique<MockHeapCursor>(data_));
  }

private:
  MockTableData &data_;
};

class MockTableStorageManager final : public mattsql::TableStorageManager {
public:
  mattsql::Result<mattsql::PageId>
  CreateHeap(mattsql::Transaction &transaction,
             const mattsql::TableInfo &table) override {
    (void)transaction;
    ++create_heap_calls;
    if (fail_create_heap) {
      return mattsql::error_result<mattsql::PageId>(mattsql::ErrorCode::IoError,
                                                    "create heap failure");
    }

    MockTableData data;
    data.table_id = table.id;
    data.root_page_id = next_root_page_id++;
    tables.emplace(table.id, std::move(data));
    return mattsql::ok_result(tables.at(table.id).root_page_id);
  }

  mattsql::Result<std::unique_ptr<mattsql::HeapTable>>
  OpenHeap(const mattsql::TableStorageReference &reference) override {
    ++open_heap_calls;
    last_opened = reference;
    if (fail_open_heap) {
      return mattsql::error_result<std::unique_ptr<mattsql::HeapTable>>(
          mattsql::ErrorCode::IoError, "open heap failure");
    }

    const auto table_iter = tables.find(reference.table_id);
    if (table_iter == tables.end()) {
      return mattsql::error_result<std::unique_ptr<mattsql::HeapTable>>(
          mattsql::ErrorCode::NotFound, "heap not found");
    }

    return mattsql::ok_result<std::unique_ptr<mattsql::HeapTable>>(
        std::make_unique<MockHeapTable>(table_iter->second));
  }

  std::size_t RowCount(mattsql::TableId table_id) const {
    return tables.at(table_id).records.size();
  }

  int create_heap_calls = 0;
  int open_heap_calls = 0;
  bool fail_create_heap = false;
  bool fail_open_heap = false;
  mattsql::PageId next_root_page_id = 100;
  mattsql::TableStorageReference last_opened;
  std::unordered_map<mattsql::TableId, MockTableData> tables;
};

mattsql::StatementPtr parse_statement(const std::string &sql) {
  mattsql::Lexer lexer(sql);
  mattsql::Parser parser(lexer.Tokenize());
  return parser.ParseStatement();
}

mattsql::Result<mattsql::PhysicalPlanPtr> physical_plan_sql(const std::string &sql,
                                                            mattsql::Catalog &catalog) {
  const auto statement = parse_statement(sql);

  mattsql::DefaultBinder binder;
  auto bound = binder.Bind(*statement, catalog);
  if (!mattsql::status_ok(bound.status)) {
    return mattsql::error_result<mattsql::PhysicalPlanPtr>(bound.status);
  }

  mattsql::DefaultLogicalPlanner logical_planner;
  auto logical = logical_planner.Plan(**bound.value);
  if (!mattsql::status_ok(logical.status)) {
    return mattsql::error_result<mattsql::PhysicalPlanPtr>(logical.status);
  }

  mattsql::DefaultOptimizer optimizer;
  auto optimized = optimizer.Optimize(std::move(*logical.value));
  if (!mattsql::status_ok(optimized.status)) {
    return mattsql::error_result<mattsql::PhysicalPlanPtr>(optimized.status);
  }

  mattsql::DefaultPhysicalPlanner physical_planner;
  return physical_planner.Plan(**optimized.value);
}

mattsql::Result<mattsql::QueryResult> execute_sql(const std::string &sql,
                                                  mattsql::Catalog &catalog,
                                                  MockTableStorageManager &storage,
                                                  mattsql::Transaction &transaction) {
  auto physical = physical_plan_sql(sql, catalog);
  if (!mattsql::status_ok(physical.status)) {
    return mattsql::error_result<mattsql::QueryResult>(physical.status);
  }

  mattsql::DefaultExecutor executor(catalog, storage);
  return executor.Execute(**physical.value, transaction);
}

} // namespace

/// Verifies the executor can run the full hosted SQL pipeline over heap storage.
TEST_CASE(executor_creates_inserts_and_selects_rows) {
  mattsql::InMemoryCatalog catalog;
  MockTableStorageManager storage;
  MockTransaction transaction;

  auto create = execute_sql("CREATE TABLE users (id INT, name TEXT, active BOOL);",
                            catalog, storage, transaction);
  EXPECT_TRUE(mattsql::status_ok(create.status));
  EXPECT_EQ(storage.create_heap_calls, 1);

  const auto table = catalog.GetTable("users");
  EXPECT_TRUE(mattsql::status_ok(table.status));
  EXPECT_EQ(table.value->heap_root_page_id, mattsql::PageId{100});

  EXPECT_TRUE(mattsql::status_ok(execute_sql("INSERT INTO users VALUES (1, 'Ada', 1);",
                                             catalog, storage, transaction)
                                     .status));
  EXPECT_TRUE(
      mattsql::status_ok(execute_sql("INSERT INTO users VALUES (2, 'Grace', 0);",
                                     catalog, storage, transaction)
                             .status));
  EXPECT_EQ(storage.RowCount(table.value->id), 2U);

  const auto result =
      execute_sql("SELECT id AS user_id, name FROM users WHERE active = 1;", catalog,
                  storage, transaction);

  EXPECT_TRUE(mattsql::status_ok(result.status));
  EXPECT_EQ(result.value->columns.size(), 2U);
  EXPECT_EQ(result.value->columns[0], std::string("user_id"));
  EXPECT_EQ(result.value->columns[1], std::string("name"));
  EXPECT_EQ(result.value->rows.size(), 1U);
  EXPECT_EQ(std::get<std::int64_t>(result.value->rows[0][0]), std::int64_t{1});
  EXPECT_EQ(std::get<std::string>(result.value->rows[0][1]), std::string("Ada"));
  EXPECT_EQ(storage.last_opened.root_page_id, mattsql::PageId{100});
}

/// Verifies scalar SELECT execution evaluates expressions without storage rows.
TEST_CASE(executor_executes_scalar_projection) {
  mattsql::InMemoryCatalog catalog;
  MockTableStorageManager storage;
  MockTransaction transaction;

  const auto result =
      execute_sql("SELECT 1 + 2 * 3, 'x';", catalog, storage, transaction);

  EXPECT_TRUE(mattsql::status_ok(result.status));
  EXPECT_EQ(result.value->columns.size(), 2U);
  EXPECT_EQ(result.value->columns[0], std::string("expr1"));
  EXPECT_EQ(result.value->columns[1], std::string("expr2"));
  EXPECT_EQ(result.value->rows.size(), 1U);
  EXPECT_EQ(std::get<std::int64_t>(result.value->rows[0][0]), std::int64_t{7});
  EXPECT_EQ(std::get<std::string>(result.value->rows[0][1]), std::string("x"));
  EXPECT_EQ(storage.open_heap_calls, 0);
}

/// Verifies optimized empty table inputs materialize as an empty result.
TEST_CASE(executor_executes_optimized_empty_filter_result) {
  mattsql::InMemoryCatalog catalog;
  MockTableStorageManager storage;
  MockTransaction transaction;

  EXPECT_TRUE(mattsql::status_ok(
      execute_sql("CREATE TABLE users (id INT, name TEXT, active BOOL);", catalog,
                  storage, transaction)
          .status));

  const auto result =
      execute_sql("SELECT id FROM users WHERE 1 = 0;", catalog, storage, transaction);

  EXPECT_TRUE(mattsql::status_ok(result.status));
  EXPECT_EQ(result.value->columns.size(), 1U);
  EXPECT_EQ(result.value->columns[0], std::string("id"));
  EXPECT_EQ(result.value->rows.size(), 0U);
}

/// Verifies tuple encoding rejects bad rows and round-trips valid rows.
TEST_CASE(binary_tuple_codec_validates_and_round_trips_rows) {
  mattsql::TableSchema schema;
  schema.columns.push_back({"id", mattsql::SqlType::Integer, false});
  schema.columns.push_back({"name", mattsql::SqlType::Text});
  schema.columns.push_back({"active", mattsql::SqlType::Boolean, false});

  mattsql::BinaryTupleCodec codec;
  const std::vector<mattsql::Value> row = {std::int64_t{42}, std::string("Ada"), true};

  const auto encoded = codec.Encode(schema, row);
  EXPECT_TRUE(mattsql::status_ok(encoded.status));
  const auto decoded = codec.Decode(
      schema, mattsql::ConstBufferView{std::span<const std::byte>(
                  encoded.value->bytes.data(), encoded.value->bytes.size())});

  EXPECT_TRUE(mattsql::status_ok(decoded.status));
  EXPECT_EQ(std::get<std::int64_t>((*decoded.value)[0]), std::int64_t{42});
  EXPECT_EQ(std::get<std::string>((*decoded.value)[1]), std::string("Ada"));
  EXPECT_EQ(std::get<bool>((*decoded.value)[2]), true);

  EXPECT_TRUE(codec.Encode(schema, {mattsql::NullValue{}, std::string("Ada"), true})
                  .status.code == mattsql::ErrorCode::TypeMismatch);
  EXPECT_TRUE(codec.Decode(schema, mattsql::ConstBufferView{}).status.code ==
              mattsql::ErrorCode::Corruption);
}

/// Verifies write statements are rejected in read-only transactions.
TEST_CASE(executor_rejects_writes_in_read_only_transactions) {
  mattsql::InMemoryCatalog catalog;
  MockTableStorageManager storage;
  MockTransaction read_only(mattsql::TransactionMode::ReadOnly);

  auto create =
      execute_sql("CREATE TABLE users (id INT);", catalog, storage, read_only);
  EXPECT_TRUE(create.status.code == mattsql::ErrorCode::TransactionConflict);
}

/// Verifies executor propagates storage failures and rolls back failed DDL.
TEST_CASE(executor_propagates_storage_errors) {
  {
    mattsql::InMemoryCatalog catalog;
    MockTableStorageManager storage;
    storage.fail_create_heap = true;
    MockTransaction transaction;

    const auto result =
        execute_sql("CREATE TABLE users (id INT);", catalog, storage, transaction);
    EXPECT_TRUE(result.status.code == mattsql::ErrorCode::IoError);
    EXPECT_TRUE(catalog.GetTable("users").status.code == mattsql::ErrorCode::NotFound);
  }

  {
    mattsql::InMemoryCatalog catalog;
    MockTableStorageManager storage;
    MockTransaction transaction;
    EXPECT_TRUE(mattsql::status_ok(
        execute_sql("CREATE TABLE users (id INT);", catalog, storage, transaction)
            .status));
    storage.fail_open_heap = true;

    const auto result =
        execute_sql("INSERT INTO users VALUES (1);", catalog, storage, transaction);
    EXPECT_TRUE(result.status.code == mattsql::ErrorCode::IoError);
  }
}

/// Verifies malformed physical plans fail during execution.
TEST_CASE(executor_rejects_invalid_physical_plans) {
  mattsql::InMemoryCatalog catalog;
  MockTableStorageManager storage;
  MockTransaction transaction;
  mattsql::DefaultExecutor executor(catalog, storage);

  struct UnknownPlan final : mattsql::PhysicalPlan {};
  UnknownPlan unknown;
  EXPECT_TRUE(executor.Execute(unknown, transaction).status.code ==
              mattsql::ErrorCode::ExecutionError);

  mattsql::PhysicalFilter missing_predicate;
  missing_predicate.kind = mattsql::PhysicalOperatorKind::Filter;
  missing_predicate.children.push_back(std::make_unique<mattsql::PhysicalValues>());
  EXPECT_TRUE(executor.Execute(missing_predicate, transaction).status.code ==
              mattsql::ErrorCode::ExecutionError);

  mattsql::PhysicalProjection missing_child;
  missing_child.kind = mattsql::PhysicalOperatorKind::Projection;
  auto literal = std::make_unique<mattsql::BoundLiteralExpression>();
  literal->kind = mattsql::BoundExpressionKind::Literal;
  literal->type = mattsql::SqlType::Integer;
  literal->value = std::int64_t{1};
  missing_child.projections.push_back(std::move(literal));
  EXPECT_TRUE(executor.Execute(missing_child, transaction).status.code ==
              mattsql::ErrorCode::ExecutionError);

  transaction.SetState(mattsql::TransactionState::Committed);
  mattsql::PhysicalValues values;
  values.kind = mattsql::PhysicalOperatorKind::Values;
  EXPECT_TRUE(executor.Execute(values, transaction).status.code ==
              mattsql::ErrorCode::TransactionConflict);
}
