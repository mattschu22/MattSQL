#include "mattsql/catalog/in_memory_catalog.hpp"

#include "test_framework.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

bool ok(const mattsql::Status &status) { return status.code == mattsql::ErrorCode::Ok; }

mattsql::TableSchema user_schema() {
  mattsql::TableSchema schema;
  schema.columns.push_back({"id", mattsql::SqlType::Integer, false});
  schema.columns.push_back({"name", mattsql::SqlType::Text});
  return schema;
}

class MockHostedCatalogApi final : public mattsql::HostedCatalogApi {
public:
  mattsql::Result<mattsql::TableId> AllocateTableId() override {
    ++allocate_table_id_calls;
    if (fail_allocate_table_id) {
      return error_result<mattsql::TableId>(mattsql::ErrorCode::Internal,
                                            "table id failure");
    }
    return ok_result(next_table_id++);
  }

  mattsql::Result<mattsql::IndexId> AllocateIndexId() override {
    ++allocate_index_id_calls;
    if (fail_allocate_index_id) {
      return error_result<mattsql::IndexId>(mattsql::ErrorCode::Internal,
                                            "index id failure");
    }
    return ok_result(next_index_id++);
  }

  mattsql::Result<mattsql::TableInfo>
  StoreTable(std::string_view table_key, const mattsql::TableInfo &table) override {
    ++store_table_calls;
    last_table_key = std::string(table_key);
    if (fail_store_table) {
      return error_result<mattsql::TableInfo>(mattsql::ErrorCode::IoError,
                                              "store table failure");
    }
    tables.emplace(last_table_key, table);
    table_keys_by_id.emplace(table.id, last_table_key);
    return ok_result(table);
  }

  mattsql::Status EraseTable(std::string_view table_key) override {
    ++erase_table_calls;
    last_table_key = std::string(table_key);
    const auto table_iter = tables.find(last_table_key);
    if (table_iter == tables.end()) {
      return {mattsql::ErrorCode::NotFound, "table not found"};
    }
    table_keys_by_id.erase(table_iter->second.id);
    tables.erase(table_iter);
    indexes.erase(last_table_key);
    return {};
  }

  mattsql::Result<mattsql::TableInfo>
  LoadTable(std::string_view table_key) const override {
    ++load_table_by_name_calls;
    last_table_key = std::string(table_key);
    if (fail_load_table) {
      return error_result<mattsql::TableInfo>(mattsql::ErrorCode::IoError,
                                              "load table failure");
    }
    const auto table_iter = tables.find(last_table_key);
    if (table_iter == tables.end()) {
      return error_result<mattsql::TableInfo>(mattsql::ErrorCode::NotFound,
                                              "table not found");
    }
    return ok_result(table_iter->second);
  }

  mattsql::Result<mattsql::TableInfo>
  LoadTable(mattsql::TableId table_id) const override {
    ++load_table_by_id_calls;
    const auto key_iter = table_keys_by_id.find(table_id);
    if (key_iter == table_keys_by_id.end()) {
      return error_result<mattsql::TableInfo>(mattsql::ErrorCode::NotFound,
                                              "table not found");
    }
    return ok_result(tables.at(key_iter->second));
  }

  mattsql::Result<std::vector<mattsql::TableInfo>> LoadTables() const override {
    ++load_tables_calls;
    std::vector<mattsql::TableInfo> result;
    for (const auto &[key, table] : tables) {
      (void)key;
      result.push_back(table);
    }
    return ok_result(std::move(result));
  }

  mattsql::Result<mattsql::IndexInfo>
  StoreIndex(std::string_view table_key, std::string_view index_key,
             const mattsql::IndexInfo &index) override {
    ++store_index_calls;
    last_table_key = std::string(table_key);
    last_index_key = std::string(index_key);
    if (fail_store_index) {
      return error_result<mattsql::IndexInfo>(mattsql::ErrorCode::IoError,
                                              "store index failure");
    }
    indexes[last_table_key].emplace(last_index_key, index);
    tables[last_table_key].indexes.push_back(index);
    return ok_result(index);
  }

  mattsql::Result<mattsql::IndexInfo>
  LoadIndex(std::string_view table_key, std::string_view index_key) const override {
    ++load_index_calls;
    last_table_key = std::string(table_key);
    last_index_key = std::string(index_key);
    const auto table_iter = indexes.find(last_table_key);
    if (table_iter == indexes.end()) {
      return error_result<mattsql::IndexInfo>(mattsql::ErrorCode::NotFound,
                                              "index not found");
    }
    const auto index_iter = table_iter->second.find(last_index_key);
    if (index_iter == table_iter->second.end()) {
      return error_result<mattsql::IndexInfo>(mattsql::ErrorCode::NotFound,
                                              "index not found");
    }
    return ok_result(index_iter->second);
  }

  template <typename T> static mattsql::Result<T> ok_result(T value) {
    mattsql::Result<T> result;
    result.value.emplace(std::move(value));
    return result;
  }

  template <typename T>
  static mattsql::Result<T> error_result(mattsql::ErrorCode code, std::string message) {
    mattsql::Result<T> result;
    result.status = {code, std::move(message)};
    return result;
  }

  mutable int load_table_by_name_calls = 0;
  mutable int load_table_by_id_calls = 0;
  mutable int load_tables_calls = 0;
  mutable int load_index_calls = 0;
  int allocate_table_id_calls = 0;
  int allocate_index_id_calls = 0;
  int store_table_calls = 0;
  int store_index_calls = 0;
  int erase_table_calls = 0;

  bool fail_load_table = false;
  bool fail_allocate_table_id = false;
  bool fail_allocate_index_id = false;
  bool fail_store_table = false;
  bool fail_store_index = false;

  mattsql::TableId next_table_id = 100;
  mattsql::IndexId next_index_id = 500;
  mutable std::string last_table_key;
  mutable std::string last_index_key;
  std::unordered_map<std::string, mattsql::TableInfo> tables;
  std::unordered_map<mattsql::TableId, std::string> table_keys_by_id;
  std::unordered_map<std::string, std::unordered_map<std::string, mattsql::IndexInfo>>
      indexes;
};

} // namespace

/// Verifies tables can be created, fetched by name/id, listed, and dropped.
TEST_CASE(catalog_creates_gets_lists_and_drops_tables) {
  mattsql::InMemoryCatalog catalog;

  const auto created = catalog.CreateTable({"users", user_schema()});

  EXPECT_TRUE(ok(created.status));
  EXPECT_TRUE(created.value.has_value());
  EXPECT_EQ(created.value->id, mattsql::TableId{1});
  EXPECT_EQ(created.value->name, std::string("users"));
  EXPECT_EQ(created.value->schema.columns.size(), 2U);
  EXPECT_EQ(created.value->schema.columns[0].id, mattsql::ColumnId{0});
  EXPECT_EQ(created.value->schema.columns[1].id, mattsql::ColumnId{1});
  EXPECT_TRUE(created.value->schema.columns[0].type == mattsql::SqlType::Integer);
  EXPECT_TRUE(!created.value->schema.columns[0].nullable);

  const auto by_name = catalog.GetTable("USERS");
  EXPECT_TRUE(ok(by_name.status));
  EXPECT_EQ(by_name.value->id, created.value->id);

  const auto by_id = catalog.GetTable(created.value->id);
  EXPECT_TRUE(ok(by_id.status));
  EXPECT_EQ(by_id.value->name, std::string("users"));

  const auto listed = catalog.ListTables();
  EXPECT_TRUE(ok(listed.status));
  EXPECT_EQ(listed.value->size(), 1U);
  EXPECT_EQ((*listed.value)[0].id, created.value->id);

  EXPECT_TRUE(ok(catalog.DropTable("Users")));
  EXPECT_TRUE(catalog.GetTable("users").status.code == mattsql::ErrorCode::NotFound);
  EXPECT_TRUE(catalog.GetTable(created.value->id).status.code ==
              mattsql::ErrorCode::NotFound);

  const auto recreated = catalog.CreateTable({"users", user_schema()});
  EXPECT_TRUE(ok(recreated.status));
  EXPECT_EQ(recreated.value->id, mattsql::TableId{2});
}

/// Verifies invalid table metadata is rejected before catalog mutation.
TEST_CASE(catalog_rejects_invalid_table_requests) {
  mattsql::InMemoryCatalog catalog;

  EXPECT_TRUE(catalog.CreateTable({"", user_schema()}).status.code ==
              mattsql::ErrorCode::InvalidArgument);
  EXPECT_TRUE(catalog.CreateTable({"empty", {}}).status.code ==
              mattsql::ErrorCode::InvalidArgument);

  auto missing_column_name = user_schema();
  missing_column_name.columns[0].name.clear();
  EXPECT_TRUE(catalog.CreateTable({"bad_name", missing_column_name}).status.code ==
              mattsql::ErrorCode::InvalidArgument);

  auto null_type = user_schema();
  null_type.columns[0].type = mattsql::SqlType::Null;
  EXPECT_TRUE(catalog.CreateTable({"bad_type", null_type}).status.code ==
              mattsql::ErrorCode::InvalidArgument);

  auto duplicate_columns = user_schema();
  duplicate_columns.columns[1].name = "ID";
  EXPECT_TRUE(
      catalog.CreateTable({"duplicate_columns", duplicate_columns}).status.code ==
      mattsql::ErrorCode::AlreadyExists);

  const auto users = catalog.CreateTable({"users", user_schema()});
  EXPECT_TRUE(ok(users.status));
  EXPECT_EQ(users.value->id, mattsql::TableId{1});
  EXPECT_TRUE(catalog.CreateTable({"USERS", user_schema()}).status.code ==
              mattsql::ErrorCode::AlreadyExists);
  EXPECT_TRUE(catalog.DropTable("missing").code == mattsql::ErrorCode::NotFound);

  const auto first_valid = catalog.CreateTable({"first_valid", user_schema()});
  EXPECT_TRUE(ok(first_valid.status));
  EXPECT_EQ(first_valid.value->id, mattsql::TableId{2});
}

/// Verifies indexes can be created and looked up through table metadata.
TEST_CASE(catalog_creates_and_gets_indexes) {
  mattsql::InMemoryCatalog catalog;
  const auto table = catalog.CreateTable({"users", user_schema()});
  EXPECT_TRUE(ok(table.status));

  mattsql::CreateIndexRequest request;
  request.table_name = "USERS";
  request.schema.name = "users_id_idx";
  request.schema.key_columns.push_back(0);
  request.schema.unique = true;

  const auto created = catalog.CreateIndex(request);
  EXPECT_TRUE(ok(created.status));
  EXPECT_EQ(created.value->id, mattsql::IndexId{1});
  EXPECT_EQ(created.value->table_id, table.value->id);
  EXPECT_EQ(created.value->name, std::string("users_id_idx"));
  EXPECT_TRUE(created.value->schema.unique);

  const auto found = catalog.GetIndex("users", "USERS_ID_IDX");
  EXPECT_TRUE(ok(found.status));
  EXPECT_EQ(found.value->id, created.value->id);

  const auto refreshed_table = catalog.GetTable("users");
  EXPECT_TRUE(ok(refreshed_table.status));
  EXPECT_EQ(refreshed_table.value->indexes.size(), 1U);
  EXPECT_EQ(refreshed_table.value->indexes[0].id, created.value->id);

  EXPECT_TRUE(ok(catalog.DropTable("users")));
  EXPECT_TRUE(catalog.GetIndex("users", "users_id_idx").status.code ==
              mattsql::ErrorCode::NotFound);
}

/// Verifies invalid index definitions and missing objects are rejected.
TEST_CASE(catalog_rejects_invalid_index_requests) {
  mattsql::InMemoryCatalog catalog;
  EXPECT_TRUE(ok(catalog.CreateTable({"users", user_schema()}).status));

  mattsql::CreateIndexRequest request;
  request.table_name = "users";
  request.schema.name = "users_id_idx";
  request.schema.key_columns.push_back(0);
  EXPECT_TRUE(ok(catalog.CreateIndex(request).status));

  EXPECT_TRUE(catalog.CreateIndex(request).status.code ==
              mattsql::ErrorCode::AlreadyExists);

  mattsql::CreateIndexRequest missing_table = request;
  missing_table.table_name = "missing";
  missing_table.schema.name = "missing_idx";
  EXPECT_TRUE(catalog.CreateIndex(missing_table).status.code ==
              mattsql::ErrorCode::NotFound);

  mattsql::CreateIndexRequest empty_table = request;
  empty_table.table_name.clear();
  empty_table.schema.name = "empty_table_idx";
  EXPECT_TRUE(catalog.CreateIndex(empty_table).status.code ==
              mattsql::ErrorCode::InvalidArgument);

  mattsql::CreateIndexRequest empty_name = request;
  empty_name.schema.name.clear();
  EXPECT_TRUE(catalog.CreateIndex(empty_name).status.code ==
              mattsql::ErrorCode::InvalidArgument);

  mattsql::CreateIndexRequest empty_key = request;
  empty_key.schema.name = "empty_key_idx";
  empty_key.schema.key_columns.clear();
  EXPECT_TRUE(catalog.CreateIndex(empty_key).status.code ==
              mattsql::ErrorCode::InvalidArgument);

  mattsql::CreateIndexRequest out_of_range = request;
  out_of_range.schema.name = "bad_column_idx";
  out_of_range.schema.key_columns = {2};
  EXPECT_TRUE(catalog.CreateIndex(out_of_range).status.code ==
              mattsql::ErrorCode::InvalidArgument);

  mattsql::CreateIndexRequest duplicate_key = request;
  duplicate_key.schema.name = "duplicate_key_idx";
  duplicate_key.schema.key_columns = {0, 0};
  EXPECT_TRUE(catalog.CreateIndex(duplicate_key).status.code ==
              mattsql::ErrorCode::InvalidArgument);

  EXPECT_TRUE(catalog.GetIndex("users", "missing").status.code ==
              mattsql::ErrorCode::NotFound);
  EXPECT_TRUE(catalog.GetIndex("missing", "users_id_idx").status.code ==
              mattsql::ErrorCode::NotFound);
}

/// Verifies ListTables returns deterministic table-id order.
TEST_CASE(catalog_lists_tables_in_creation_order) {
  mattsql::InMemoryCatalog catalog;

  EXPECT_TRUE(ok(catalog.CreateTable({"users", user_schema()}).status));
  EXPECT_TRUE(ok(catalog.CreateTable({"projects", user_schema()}).status));
  EXPECT_TRUE(ok(catalog.CreateTable({"events", user_schema()}).status));

  const auto listed = catalog.ListTables();
  EXPECT_TRUE(ok(listed.status));
  EXPECT_EQ(listed.value->size(), 3U);
  EXPECT_EQ((*listed.value)[0].name, std::string("users"));
  EXPECT_EQ((*listed.value)[1].name, std::string("projects"));
  EXPECT_EQ((*listed.value)[2].name, std::string("events"));
}

/// Verifies the default hosted API can store and retrieve raw catalog metadata.
TEST_CASE(hosted_catalog_api_stores_tables_and_indexes) {
  mattsql::InMemoryHostedCatalogApi api;

  const auto table_id = api.AllocateTableId();
  EXPECT_TRUE(ok(table_id.status));
  EXPECT_EQ(*table_id.value, mattsql::TableId{1});

  mattsql::TableInfo table;
  table.id = *table_id.value;
  table.name = "users";
  table.schema = user_schema();
  EXPECT_TRUE(ok(api.StoreTable("users", table).status));

  const auto by_name = api.LoadTable("users");
  EXPECT_TRUE(ok(by_name.status));
  EXPECT_EQ(by_name.value->id, table.id);

  const auto by_id = api.LoadTable(table.id);
  EXPECT_TRUE(ok(by_id.status));
  EXPECT_EQ(by_id.value->name, std::string("users"));

  const auto index_id = api.AllocateIndexId();
  EXPECT_TRUE(ok(index_id.status));
  EXPECT_EQ(*index_id.value, mattsql::IndexId{1});

  mattsql::IndexInfo index;
  index.id = *index_id.value;
  index.name = "users_id_idx";
  index.table_id = table.id;
  index.schema.name = "users_id_idx";
  index.schema.key_columns.push_back(0);
  EXPECT_TRUE(ok(api.StoreIndex("users", "users_id_idx", index).status));

  const auto found_index = api.LoadIndex("users", "users_id_idx");
  EXPECT_TRUE(ok(found_index.status));
  EXPECT_EQ(found_index.value->id, index.id);

  const auto listed = api.LoadTables();
  EXPECT_TRUE(ok(listed.status));
  EXPECT_EQ(listed.value->size(), 1U);
  EXPECT_EQ((*listed.value)[0].indexes.size(), 1U);

  EXPECT_TRUE(ok(api.EraseTable("users")));
  EXPECT_TRUE(api.LoadTable("users").status.code == mattsql::ErrorCode::NotFound);
  EXPECT_TRUE(api.LoadIndex("users", "users_id_idx").status.code ==
              mattsql::ErrorCode::NotFound);
}

/// Verifies the default hosted API rejects duplicate or missing metadata.
TEST_CASE(hosted_catalog_api_rejects_invalid_operations) {
  mattsql::InMemoryHostedCatalogApi api;

  mattsql::TableInfo table;
  table.id = 1;
  table.name = "users";
  table.schema = user_schema();
  EXPECT_TRUE(ok(api.StoreTable("users", table).status));
  EXPECT_TRUE(api.StoreTable("users", table).status.code ==
              mattsql::ErrorCode::AlreadyExists);
  EXPECT_TRUE(api.LoadTable("missing").status.code == mattsql::ErrorCode::NotFound);
  EXPECT_TRUE(api.LoadTable(mattsql::TableId{99}).status.code ==
              mattsql::ErrorCode::NotFound);
  EXPECT_TRUE(api.EraseTable("missing").code == mattsql::ErrorCode::NotFound);

  mattsql::IndexInfo index;
  index.id = 1;
  index.name = "users_id_idx";
  index.table_id = 1;
  EXPECT_TRUE(ok(api.StoreIndex("users", "users_id_idx", index).status));
  EXPECT_TRUE(api.StoreIndex("users", "users_id_idx", index).status.code ==
              mattsql::ErrorCode::AlreadyExists);
  EXPECT_TRUE(api.StoreIndex("missing", "idx", index).status.code ==
              mattsql::ErrorCode::NotFound);
  EXPECT_TRUE(api.LoadIndex("users", "missing").status.code ==
              mattsql::ErrorCode::NotFound);
}

/// Verifies InMemoryCatalog routes hosted operations through the injected API.
TEST_CASE(catalog_uses_mock_hosted_api) {
  auto mock = std::make_unique<MockHostedCatalogApi>();
  auto *api = mock.get();
  mattsql::InMemoryCatalog catalog(std::move(mock));

  const auto created = catalog.CreateTable({"Users", user_schema()});
  EXPECT_TRUE(ok(created.status));
  EXPECT_EQ(created.value->id, mattsql::TableId{100});
  EXPECT_EQ(api->load_table_by_name_calls, 1);
  EXPECT_EQ(api->allocate_table_id_calls, 1);
  EXPECT_EQ(api->store_table_calls, 1);
  EXPECT_EQ(api->last_table_key, std::string("users"));

  const auto by_name = catalog.GetTable("USERS");
  EXPECT_TRUE(ok(by_name.status));
  EXPECT_EQ(by_name.value->id, created.value->id);
  EXPECT_EQ(api->last_table_key, std::string("users"));

  mattsql::CreateIndexRequest request;
  request.table_name = "users";
  request.schema.name = "Users_ID_Idx";
  request.schema.key_columns.push_back(0);
  const auto index = catalog.CreateIndex(request);
  EXPECT_TRUE(ok(index.status));
  EXPECT_EQ(index.value->id, mattsql::IndexId{500});
  EXPECT_EQ(api->allocate_index_id_calls, 1);
  EXPECT_EQ(api->store_index_calls, 1);
  EXPECT_EQ(api->last_table_key, std::string("users"));
  EXPECT_EQ(api->last_index_key, std::string("users_id_idx"));
}

/// Verifies catalog methods propagate hosted API failures without hiding them.
TEST_CASE(catalog_propagates_mock_hosted_api_errors) {
  {
    auto mock = std::make_unique<MockHostedCatalogApi>();
    mock->fail_load_table = true;
    mattsql::InMemoryCatalog catalog(std::move(mock));

    EXPECT_TRUE(catalog.CreateTable({"users", user_schema()}).status.code ==
                mattsql::ErrorCode::IoError);
  }

  {
    auto mock = std::make_unique<MockHostedCatalogApi>();
    mock->fail_allocate_table_id = true;
    mattsql::InMemoryCatalog catalog(std::move(mock));

    EXPECT_TRUE(catalog.CreateTable({"users", user_schema()}).status.code ==
                mattsql::ErrorCode::Internal);
  }

  {
    auto mock = std::make_unique<MockHostedCatalogApi>();
    mock->fail_store_table = true;
    mattsql::InMemoryCatalog catalog(std::move(mock));

    EXPECT_TRUE(catalog.CreateTable({"users", user_schema()}).status.code ==
                mattsql::ErrorCode::IoError);
  }

  {
    auto mock = std::make_unique<MockHostedCatalogApi>();
    auto *api = mock.get();
    mattsql::InMemoryCatalog catalog(std::move(mock));
    EXPECT_TRUE(ok(catalog.CreateTable({"users", user_schema()}).status));
    api->fail_allocate_index_id = true;

    mattsql::CreateIndexRequest request;
    request.table_name = "users";
    request.schema.name = "idx";
    request.schema.key_columns.push_back(0);
    EXPECT_TRUE(catalog.CreateIndex(request).status.code ==
                mattsql::ErrorCode::Internal);
  }

  {
    auto mock = std::make_unique<MockHostedCatalogApi>();
    auto *api = mock.get();
    mattsql::InMemoryCatalog catalog(std::move(mock));
    EXPECT_TRUE(ok(catalog.CreateTable({"users", user_schema()}).status));
    api->fail_store_index = true;

    mattsql::CreateIndexRequest request;
    request.table_name = "users";
    request.schema.name = "idx";
    request.schema.key_columns.push_back(0);
    EXPECT_TRUE(catalog.CreateIndex(request).status.code ==
                mattsql::ErrorCode::IoError);
  }
}
