#include "mattsql/binder/default_binder.hpp"
#include "mattsql/catalog/in_memory_catalog.hpp"
#include "mattsql/common/result_utils.hpp"
#include "mattsql/execution/default_executor.hpp"
#include "mattsql/lexer/lexer.hpp"
#include "mattsql/optimizer/default_optimizer.hpp"
#include "mattsql/parser/parser.hpp"
#include "mattsql/planner/default_logical_planner.hpp"
#include "mattsql/planner/default_physical_planner.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace {

class CliTransaction final : public mattsql::Transaction {
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

struct CliTableData {
  mattsql::TableId table_id = 0;
  mattsql::PageId root_page_id = mattsql::kInvalidPageId;
  std::vector<std::vector<std::byte>> records;
};

class CliHeapCursor final : public mattsql::HeapCursor {
public:
  explicit CliHeapCursor(const CliTableData &table) : table_(table) {}

  mattsql::Result<mattsql::RecordView> Next() override {
    if (index_ >= table_.records.size()) {
      return mattsql::error_result<mattsql::RecordView>(mattsql::ErrorCode::NotFound,
                                                        "end of heap scan");
    }

    const auto &record = table_.records[index_];
    mattsql::RecordView view;
    view.id.page_id = table_.root_page_id;
    view.id.slot_id = static_cast<mattsql::SlotId>(index_);
    view.bytes = mattsql::ConstBufferView{
        std::span<const std::byte>(record.data(), record.size())};
    ++index_;
    return mattsql::ok_result(view);
  }

private:
  const CliTableData &table_;
  std::size_t index_ = 0;
};

class CliHeapTable final : public mattsql::HeapTable {
public:
  explicit CliHeapTable(CliTableData &table) : table_(table) {}

  mattsql::Result<mattsql::RecordId> Insert(mattsql::Transaction &transaction,
                                            mattsql::ConstBufferView record) override {
    (void)transaction;

    std::vector<std::byte> bytes(record.bytes.begin(), record.bytes.end());
    table_.records.push_back(std::move(bytes));

    mattsql::RecordId id;
    id.page_id = table_.root_page_id;
    id.slot_id = static_cast<mattsql::SlotId>(table_.records.size() - 1);
    return mattsql::ok_result(id);
  }

  mattsql::Result<mattsql::RecordView> Read(mattsql::Transaction &transaction,
                                            mattsql::RecordId record_id) override {
    (void)transaction;

    if (record_id.slot_id >= table_.records.size()) {
      return mattsql::error_result<mattsql::RecordView>(mattsql::ErrorCode::NotFound,
                                                        "record not found");
    }

    const auto &record = table_.records[record_id.slot_id];
    mattsql::RecordView view;
    view.id = record_id;
    view.bytes = mattsql::ConstBufferView{
        std::span<const std::byte>(record.data(), record.size())};
    return mattsql::ok_result(view);
  }

  mattsql::Status Delete(mattsql::Transaction &transaction,
                         mattsql::RecordId record_id) override {
    (void)transaction;
    if (record_id.slot_id >= table_.records.size()) {
      return mattsql::error_status(mattsql::ErrorCode::NotFound, "record not found");
    }

    table_.records.erase(table_.records.begin() + record_id.slot_id);
    return mattsql::ok_status();
  }

  mattsql::Result<std::unique_ptr<mattsql::HeapCursor>>
  Scan(mattsql::Transaction &transaction) override {
    (void)transaction;
    return mattsql::ok_result<std::unique_ptr<mattsql::HeapCursor>>(
        std::make_unique<CliHeapCursor>(table_));
  }

private:
  CliTableData &table_;
};

// The CLI keeps heap data in the session so interactive statements can observe
// previous CREATE and INSERT work until the durable storage layer is available.
class CliTableStorageManager final : public mattsql::TableStorageManager {
public:
  mattsql::Result<mattsql::PageId>
  CreateHeap(mattsql::Transaction &transaction,
             const mattsql::TableInfo &table) override {
    (void)transaction;

    if (tables_.contains(table.id)) {
      return mattsql::error_result<mattsql::PageId>(mattsql::ErrorCode::AlreadyExists,
                                                    "heap already exists");
    }

    CliTableData data;
    data.table_id = table.id;
    data.root_page_id = next_root_page_id_++;
    tables_.emplace(table.id, std::move(data));
    return mattsql::ok_result(tables_.at(table.id).root_page_id);
  }

  mattsql::Result<std::unique_ptr<mattsql::HeapTable>>
  OpenHeap(const mattsql::TableStorageReference &reference) override {
    const auto table_iter = tables_.find(reference.table_id);
    if (table_iter == tables_.end()) {
      return mattsql::error_result<std::unique_ptr<mattsql::HeapTable>>(
          mattsql::ErrorCode::NotFound, "heap not found");
    }
    if (reference.root_page_id != table_iter->second.root_page_id) {
      return mattsql::error_result<std::unique_ptr<mattsql::HeapTable>>(
          mattsql::ErrorCode::Corruption, "heap root page mismatch");
    }

    return mattsql::ok_result<std::unique_ptr<mattsql::HeapTable>>(
        std::make_unique<CliHeapTable>(table_iter->second));
  }

private:
  mattsql::PageId next_root_page_id_ = 1;
  std::unordered_map<mattsql::TableId, CliTableData> tables_;
};

struct Session {
  mattsql::InMemoryCatalog catalog;
  CliTableStorageManager storage;
  CliTransaction transaction;
};

[[nodiscard]] bool stdin_is_tty() {
#if defined(_WIN32)
  return false;
#else
  return isatty(STDIN_FILENO) == 1;
#endif
}

[[nodiscard]] std::string trim(std::string_view value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) {
    return {};
  }

  const auto last = value.find_last_not_of(" \t\r\n");
  return std::string(value.substr(first, last - first + 1));
}

[[nodiscard]] bool is_exit_command(std::string_view value) {
  const auto command = trim(value);
  return command == ".exit" || command == ".quit" || command == "exit" ||
         command == "quit";
}

// Parser currently accepts one statement at a time, so the CLI splits scripts on
// semicolons that are outside literals and comments.
[[nodiscard]] bool extract_statement(std::string &buffer, std::string &statement) {
  bool in_string = false;
  bool in_line_comment = false;
  bool in_block_comment = false;

  for (std::size_t index = 0; index < buffer.size(); ++index) {
    const auto current = buffer[index];
    const auto next = index + 1 < buffer.size() ? buffer[index + 1] : '\0';

    if (in_line_comment) {
      if (current == '\n') {
        in_line_comment = false;
      }
      continue;
    }

    if (in_block_comment) {
      if (current == '*' && next == '/') {
        ++index;
        in_block_comment = false;
      }
      continue;
    }

    if (in_string) {
      if (current == '\'') {
        if (next == '\'') {
          ++index;
          continue;
        }
        in_string = false;
      }
      continue;
    }

    if (current == '\'') {
      in_string = true;
      continue;
    }

    if (current == '-' && next == '-') {
      ++index;
      in_line_comment = true;
      continue;
    }

    if (current == '/' && next == '*') {
      ++index;
      in_block_comment = true;
      continue;
    }

    if (current == ';') {
      statement = buffer.substr(0, index + 1);
      buffer.erase(0, index + 1);
      return true;
    }
  }

  return false;
}

[[nodiscard]] std::string error_code_name(mattsql::ErrorCode code) {
  switch (code) {
  case mattsql::ErrorCode::Ok:
    return "Ok";
  case mattsql::ErrorCode::InvalidArgument:
    return "InvalidArgument";
  case mattsql::ErrorCode::NotFound:
    return "NotFound";
  case mattsql::ErrorCode::AlreadyExists:
    return "AlreadyExists";
  case mattsql::ErrorCode::TypeMismatch:
    return "TypeMismatch";
  case mattsql::ErrorCode::ParseError:
    return "ParseError";
  case mattsql::ErrorCode::BindError:
    return "BindError";
  case mattsql::ErrorCode::PlanError:
    return "PlanError";
  case mattsql::ErrorCode::ExecutionError:
    return "ExecutionError";
  case mattsql::ErrorCode::IoError:
    return "IoError";
  case mattsql::ErrorCode::Corruption:
    return "Corruption";
  case mattsql::ErrorCode::TransactionConflict:
    return "TransactionConflict";
  case mattsql::ErrorCode::NotSupported:
    return "NotSupported";
  case mattsql::ErrorCode::Internal:
    return "Internal";
  }

  return "Unknown";
}

[[nodiscard]] std::string format_value(const mattsql::Value &value) {
  if (std::holds_alternative<mattsql::NullValue>(value)) {
    return "NULL";
  }
  if (const auto *integer = std::get_if<std::int64_t>(&value)) {
    return std::to_string(*integer);
  }
  if (const auto *boolean = std::get_if<bool>(&value)) {
    return *boolean ? "true" : "false";
  }
  return std::get<std::string>(value);
}

void print_result(const mattsql::QueryResult &result) {
  if (result.columns.empty()) {
    std::cout << "OK\n";
    return;
  }

  for (std::size_t index = 0; index < result.columns.size(); ++index) {
    if (index != 0) {
      std::cout << '\t';
    }
    std::cout << result.columns[index];
  }
  std::cout << '\n';

  for (const auto &row : result.rows) {
    for (std::size_t index = 0; index < row.size(); ++index) {
      if (index != 0) {
        std::cout << '\t';
      }
      std::cout << format_value(row[index]);
    }
    std::cout << '\n';
  }
}

[[nodiscard]] mattsql::Result<mattsql::QueryResult>
execute_statement(Session &session, const std::string &sql) {
  mattsql::Lexer lexer(sql);
  mattsql::Parser parser(lexer.Tokenize());
  const auto statement = parser.ParseStatement();

  // Main uses the staged interfaces directly instead of the deprecated Engine
  // wrapper, which keeps the executable aligned with the current architecture.
  mattsql::DefaultBinder binder;
  auto bound = binder.Bind(*statement, session.catalog);
  if (!mattsql::status_ok(bound.status)) {
    return mattsql::error_result<mattsql::QueryResult>(std::move(bound.status));
  }

  mattsql::DefaultLogicalPlanner logical_planner;
  auto logical = logical_planner.Plan(**bound.value);
  if (!mattsql::status_ok(logical.status)) {
    return mattsql::error_result<mattsql::QueryResult>(std::move(logical.status));
  }

  mattsql::DefaultOptimizer optimizer;
  auto optimized = optimizer.Optimize(std::move(*logical.value));
  if (!mattsql::status_ok(optimized.status)) {
    return mattsql::error_result<mattsql::QueryResult>(std::move(optimized.status));
  }

  mattsql::DefaultPhysicalPlanner physical_planner;
  auto physical = physical_planner.Plan(**optimized.value);
  if (!mattsql::status_ok(physical.status)) {
    return mattsql::error_result<mattsql::QueryResult>(std::move(physical.status));
  }

  mattsql::DefaultExecutor executor(session.catalog, session.storage);
  return executor.Execute(**physical.value, session.transaction);
}

[[nodiscard]] bool run_statement(Session &session, const std::string &sql) {
  const auto trimmed = trim(sql);
  if (trimmed.empty()) {
    return true;
  }

  try {
    auto result = execute_statement(session, trimmed);
    if (!mattsql::status_ok(result.status)) {
      std::cerr << "error[" << error_code_name(result.status.code)
                << "]: " << result.status.message << '\n';
      return false;
    }

    print_result(*result.value);
    return true;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return false;
  }
}

[[nodiscard]] bool run_sql_text(Session &session, std::string sql) {
  bool success = true;
  std::string statement;
  while (extract_statement(sql, statement)) {
    success = run_statement(session, statement) && success;
  }

  if (!trim(sql).empty()) {
    success = run_statement(session, sql) && success;
  }

  return success;
}

int run_repl() {
  Session session;
  std::string buffer;
  std::string line;

  std::cout
      << "MattSQL interactive shell. End statements with ';'. Type .quit to exit.\n";
  while (true) {
    std::cout << (trim(buffer).empty() ? "mattsql> " : "      -> ");
    std::cout.flush();

    if (!std::getline(std::cin, line)) {
      std::cout << '\n';
      break;
    }

    if (trim(buffer).empty() && is_exit_command(line)) {
      break;
    }

    buffer.append(line);
    buffer.push_back('\n');

    std::string statement;
    while (extract_statement(buffer, statement)) {
      (void)run_statement(session, statement);
    }
  }

  if (!trim(buffer).empty()) {
    (void)run_statement(session, buffer);
  }

  return 0;
}

} // namespace

int main(int argc, char **argv) {
  Session session;

  if (argc > 1) {
    std::ostringstream sql;
    for (int index = 1; index < argc; ++index) {
      if (index != 1) {
        sql << ' ';
      }
      sql << argv[index];
    }
    return run_sql_text(session, sql.str()) ? 0 : 1;
  }

  if (stdin_is_tty()) {
    return run_repl();
  }

  std::ostringstream sql;
  sql << std::cin.rdbuf();
  return run_sql_text(session, sql.str()) ? 0 : 1;
}
