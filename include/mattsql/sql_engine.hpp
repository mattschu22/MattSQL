#pragma once

#include "mattsql/catalog/catalog.hpp"
#include "mattsql/common/query_result.hpp"
#include "mattsql/common/status.hpp"
#include "mattsql/runtime/platform.hpp"
#include "mattsql/storage/table_storage.hpp"
#include "mattsql/txn/transaction.hpp"

#include <memory>
#include <string_view>

namespace mattsql {

class SqlEngine {
public:
  /// Destroys a SQL engine through the interface pointer.
  virtual ~SqlEngine() = default;

  /// Executes one SQL statement using the engine's default transaction policy.
  virtual Result<QueryResult> Execute(std::string_view sql) = 0;

  /// Executes one SQL statement inside an existing transaction.
  virtual Result<QueryResult> Execute(std::string_view sql,
                                      Transaction &transaction) = 0;

  /// Returns the catalog interface used by this engine.
  virtual Catalog &GetCatalog() = 0;

  /// Returns the runtime boundary used by this engine.
  virtual PlatformRuntime &GetRuntime() = 0;
};

class DefaultSqlEngine final : public SqlEngine {
public:
  /// Creates a hosted in-memory engine suitable for tests and the CLI.
  DefaultSqlEngine();

  /// Creates an engine over caller-owned components.
  DefaultSqlEngine(Catalog &catalog, TableStorageManager &storage,
                   PlatformRuntime &runtime);

  DefaultSqlEngine(const DefaultSqlEngine &) = delete;
  DefaultSqlEngine &operator=(const DefaultSqlEngine &) = delete;

  DefaultSqlEngine(DefaultSqlEngine &&) noexcept;
  DefaultSqlEngine &operator=(DefaultSqlEngine &&) noexcept;
  ~DefaultSqlEngine() override;

  /// Executes one SQL statement using a default read-write transaction.
  Result<QueryResult> Execute(std::string_view sql) override;

  /// Executes one SQL statement inside an existing transaction.
  Result<QueryResult> Execute(std::string_view sql, Transaction &transaction) override;

  /// Returns the catalog interface used by this engine.
  Catalog &GetCatalog() override;

  /// Returns the table storage interface used by this engine.
  TableStorageManager &GetStorage();

  /// Returns the runtime boundary used by this engine.
  PlatformRuntime &GetRuntime() override;

private:
  struct Components;
  std::unique_ptr<Components> components_;
};

} // namespace mattsql
