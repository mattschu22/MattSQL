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

DefaultSqlEngine::DefaultSqlEngine()
    : owned_catalog_(std::make_unique<InMemoryCatalog>()),
      owned_storage_(std::make_unique<InMemoryTableStorageManager>()),
      owned_runtime_(std::make_unique<HostedPlatformRuntime>()),
      catalog_(owned_catalog_.get()), storage_(owned_storage_.get()),
      runtime_(owned_runtime_.get()) {}

DefaultSqlEngine::DefaultSqlEngine(Catalog &catalog, TableStorageManager &storage,
                                   PlatformRuntime &runtime)
    : catalog_(&catalog), storage_(&storage), runtime_(&runtime) {}

DefaultSqlEngine::DefaultSqlEngine(DefaultSqlEngine &&other) noexcept
    : owned_catalog_(std::move(other.owned_catalog_)),
      owned_storage_(std::move(other.owned_storage_)),
      owned_runtime_(std::move(other.owned_runtime_)),
      catalog_(owned_catalog_ != nullptr ? owned_catalog_.get() : other.catalog_),
      storage_(owned_storage_ != nullptr ? owned_storage_.get() : other.storage_),
      runtime_(owned_runtime_ != nullptr ? owned_runtime_.get() : other.runtime_) {
  other.catalog_ = nullptr;
  other.storage_ = nullptr;
  other.runtime_ = nullptr;
}

DefaultSqlEngine &DefaultSqlEngine::operator=(DefaultSqlEngine &&other) noexcept {
  if (this == &other) {
    return *this;
  }

  const auto other_catalog = other.catalog_;
  const auto other_storage = other.storage_;
  const auto other_runtime = other.runtime_;

  owned_catalog_ = std::move(other.owned_catalog_);
  owned_storage_ = std::move(other.owned_storage_);
  owned_runtime_ = std::move(other.owned_runtime_);
  catalog_ = owned_catalog_ != nullptr ? owned_catalog_.get() : other_catalog;
  storage_ = owned_storage_ != nullptr ? owned_storage_.get() : other_storage;
  runtime_ = owned_runtime_ != nullptr ? owned_runtime_.get() : other_runtime;

  other.catalog_ = nullptr;
  other.storage_ = nullptr;
  other.runtime_ = nullptr;
  return *this;
}

DefaultSqlEngine::~DefaultSqlEngine() = default;

Result<QueryResult> DefaultSqlEngine::Execute(std::string_view sql) {
  DefaultStatementTransaction transaction;
  return Execute(sql, transaction);
}

Result<QueryResult> DefaultSqlEngine::Execute(std::string_view sql,
                                              Transaction &transaction) {
  if (catalog_ == nullptr || storage_ == nullptr || runtime_ == nullptr) {
    return error_result<QueryResult>(ErrorCode::Internal,
                                     "SQL engine is not initialized");
  }

  auto statement = parse_sql(sql);
  if (!status_ok(statement.status)) {
    return error_result<QueryResult>(std::move(statement.status));
  }

  DefaultBinder binder;
  auto bound = binder.Bind(**statement.value, *catalog_);
  if (!status_ok(bound.status)) {
    return error_result<QueryResult>(std::move(bound.status));
  }

  DefaultLogicalPlanner logical_planner;
  auto logical = logical_planner.Plan(**bound.value);
  if (!status_ok(logical.status)) {
    return error_result<QueryResult>(std::move(logical.status));
  }

  DefaultOptimizer optimizer;
  auto optimized = optimizer.Optimize(std::move(*logical.value));
  if (!status_ok(optimized.status)) {
    return error_result<QueryResult>(std::move(optimized.status));
  }

  DefaultPhysicalPlanner physical_planner;
  auto physical = physical_planner.Plan(**optimized.value);
  if (!status_ok(physical.status)) {
    return error_result<QueryResult>(std::move(physical.status));
  }

  DefaultExecutor executor(*catalog_, *storage_);
  return executor.Execute(**physical.value, transaction);
}

Catalog &DefaultSqlEngine::GetCatalog() { return *catalog_; }

TableStorageManager &DefaultSqlEngine::GetStorage() { return *storage_; }

PlatformRuntime &DefaultSqlEngine::GetRuntime() { return *runtime_; }

} // namespace mattsql
