#include "mattsql/sql_engine.hpp"

#include "mattsql/binder/default_binder.hpp"
#include "mattsql/catalog/in_memory_catalog.hpp"
#include "mattsql/common/result_utils.hpp"
#include "mattsql/execution/default_executor.hpp"
#include "mattsql/lexer/lexer.hpp"
#include "mattsql/optimizer/default_optimizer.hpp"
#include "mattsql/parser/parse_error.hpp"
#include "mattsql/parser/parser.hpp"
#include "mattsql/planner/default_logical_planner.hpp"
#include "mattsql/planner/default_physical_planner.hpp"
#include "mattsql/runtime/hosted_platform_runtime.hpp"
#include "mattsql/storage/in_memory_table_storage.hpp"

#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace mattsql {
namespace {

class DefaultStatementTransaction final : public Transaction {
public:
  explicit DefaultStatementTransaction(
      TransactionMode mode = TransactionMode::ReadWrite)
      : mode_(mode) {}

  TransactionId Id() const override { return 1; }
  TransactionMode Mode() const override { return mode_; }
  TransactionState State() const override { return TransactionState::Active; }
  LogSequenceNumber BeginLsn() const override { return LogSequenceNumber{0}; }

private:
  TransactionMode mode_;
};

[[nodiscard]] Result<StatementPtr> parse_sql(std::string_view sql) {
  try {
    Lexer lexer(sql);
    Parser parser(lexer.Tokenize());
    return ok_result(parser.ParseStatement());
  } catch (const ParseError &error) {
    return error_result<StatementPtr>(ErrorCode::ParseError, error.Message());
  } catch (const std::exception &error) {
    return error_result<StatementPtr>(ErrorCode::ParseError, error.what());
  }
}

} // namespace

struct DefaultSqlEngine::Components {
  std::unique_ptr<Catalog> owned_catalog;
  std::unique_ptr<TableStorageManager> owned_storage;
  std::unique_ptr<PlatformRuntime> owned_runtime;
  Catalog *catalog = nullptr;
  TableStorageManager *storage = nullptr;
  PlatformRuntime *runtime = nullptr;
};

DefaultSqlEngine::DefaultSqlEngine() : components_(std::make_unique<Components>()) {
  components_->owned_catalog = std::make_unique<InMemoryCatalog>();
  components_->owned_storage = std::make_unique<InMemoryTableStorageManager>();
  components_->owned_runtime = std::make_unique<HostedPlatformRuntime>();
  components_->catalog = components_->owned_catalog.get();
  components_->storage = components_->owned_storage.get();
  components_->runtime = components_->owned_runtime.get();
}

DefaultSqlEngine::DefaultSqlEngine(Catalog &catalog, TableStorageManager &storage,
                                   PlatformRuntime &runtime)
    : components_(std::make_unique<Components>()) {
  components_->catalog = &catalog;
  components_->storage = &storage;
  components_->runtime = &runtime;
}

DefaultSqlEngine::DefaultSqlEngine(DefaultSqlEngine &&) noexcept = default;

DefaultSqlEngine &DefaultSqlEngine::operator=(DefaultSqlEngine &&) noexcept = default;

DefaultSqlEngine::~DefaultSqlEngine() = default;

Result<QueryResult> DefaultSqlEngine::Execute(std::string_view sql) {
  DefaultStatementTransaction transaction;
  return Execute(sql, transaction);
}

Result<QueryResult> DefaultSqlEngine::Execute(std::string_view sql,
                                              Transaction &transaction) {
  if (components_ == nullptr || components_->catalog == nullptr ||
      components_->storage == nullptr || components_->runtime == nullptr) {
    return error_result<QueryResult>(ErrorCode::Internal,
                                     "SQL engine is not initialized");
  }

  auto statement = parse_sql(sql);
  if (!statement.ok()) {
    return error_result<QueryResult>(std::move(statement.status));
  }

  DefaultBinder binder;
  auto bound = binder.Bind(**statement.value, *components_->catalog);
  if (!bound.ok()) {
    return error_result<QueryResult>(std::move(bound.status));
  }

  DefaultLogicalPlanner logical_planner;
  auto logical = logical_planner.Plan(**bound.value);
  if (!logical.ok()) {
    return error_result<QueryResult>(std::move(logical.status));
  }

  DefaultOptimizer optimizer;
  auto optimized = optimizer.Optimize(std::move(*logical.value));
  if (!optimized.ok()) {
    return error_result<QueryResult>(std::move(optimized.status));
  }

  DefaultPhysicalPlanner physical_planner;
  auto physical = physical_planner.Plan(**optimized.value);
  if (!physical.ok()) {
    return error_result<QueryResult>(std::move(physical.status));
  }

  DefaultExecutor executor(*components_->catalog, *components_->storage);
  return executor.Execute(**physical.value, transaction);
}

Catalog &DefaultSqlEngine::GetCatalog() { return *components_->catalog; }

TableStorageManager &DefaultSqlEngine::GetStorage() { return *components_->storage; }

PlatformRuntime &DefaultSqlEngine::GetRuntime() { return *components_->runtime; }

} // namespace mattsql
