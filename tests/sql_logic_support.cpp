#include "sql_logic_support.hpp"

#include "mattsql/common/result_utils.hpp"
#include "mattsql/sql_engine.hpp"

#include "sql_logic_format.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sql_logic {
namespace {

enum class LogicCommand {
  StatementOk,
  StatementError,
  Query,
};

struct LogicCase {
  LogicCommand command = LogicCommand::Query;
  std::optional<mattsql::ErrorCode> expected_error;
  bool sort_rows = false;
  std::string sql;
  std::vector<std::string> expected_output;
  std::filesystem::path path;
  std::size_t line = 0;
};

std::string trim(std::string_view value) {
  std::size_t first = 0;
  while (first < value.size() &&
         std::isspace(static_cast<unsigned char>(value[first])) != 0) {
    ++first;
  }

  auto last = value.size();
  while (last > first &&
         std::isspace(static_cast<unsigned char>(value[last - 1])) != 0) {
    --last;
  }

  return std::string(value.substr(first, last - first));
}

bool is_ignored_line(std::string_view line) {
  const auto stripped = trim(line);
  return stripped.empty() || stripped.starts_with('#');
}

std::string decode_expected_line(const std::string &line) {
  std::string output;
  output.reserve(line.size());

  for (std::size_t index = 0; index < line.size(); ++index) {
    if (line[index] != '\\' || index + 1 >= line.size()) {
      output.push_back(line[index]);
      continue;
    }

    const auto escaped = line[index + 1];
    if (escaped == 't') {
      output.push_back('\t');
      ++index;
    } else if (escaped == '\\') {
      output.push_back('\\');
      ++index;
    } else {
      output.push_back(line[index]);
    }
  }

  return output;
}

[[noreturn]] void fail_case(const LogicCase &test_case, const std::string &message) {
  std::ostringstream output;
  output << test_case.path << ':' << test_case.line << ": " << message;
  throw std::runtime_error(output.str());
}

LogicCase parse_header(const std::filesystem::path &path, std::size_t line,
                       const std::string &header) {
  std::istringstream stream(header);
  std::string command;
  stream >> command;

  LogicCase test_case;
  test_case.path = path;
  test_case.line = line;

  if (command == "query") {
    std::string option;
    stream >> option;
    if (option == "rowsort") {
      test_case.sort_rows = true;
    } else if (!option.empty()) {
      fail_case(test_case, "unknown query option `" + option + '`');
    }
    test_case.command = LogicCommand::Query;
    return test_case;
  }

  if (command != "statement") {
    fail_case(test_case, "expected `statement` or `query` header");
  }

  std::string expectation;
  stream >> expectation;
  if (expectation == "ok") {
    test_case.command = LogicCommand::StatementOk;
    return test_case;
  }
  if (expectation != "error") {
    fail_case(test_case, "expected statement expectation `ok` or `error`");
  }

  std::string code_name;
  stream >> code_name;
  if (code_name.empty()) {
    fail_case(test_case, "statement error requires an ErrorCode name");
  }

  auto code = mattsql::ParseErrorCodeName(code_name);
  if (!code.has_value()) {
    fail_case(test_case, "unknown ErrorCode name `" + code_name + '`');
  }

  test_case.command = LogicCommand::StatementError;
  test_case.expected_error = *code;
  return test_case;
}

std::vector<LogicCase> load_logic_cases(const std::filesystem::path &path) {
  std::ifstream input(path);
  if (!input.is_open()) {
    throw std::runtime_error("could not open SQL logic file: " + path.string());
  }

  std::vector<std::string> lines;
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    lines.push_back(line);
  }

  std::vector<LogicCase> cases;
  std::size_t index = 0;
  while (index < lines.size()) {
    while (index < lines.size() && is_ignored_line(lines[index])) {
      ++index;
    }
    if (index >= lines.size()) {
      break;
    }

    auto test_case = parse_header(path, index + 1, trim(lines[index]));
    ++index;

    std::ostringstream sql;
    while (index < lines.size() && trim(lines[index]) != "----") {
      sql << lines[index] << '\n';
      ++index;
    }
    if (index >= lines.size()) {
      fail_case(test_case, "missing `----` delimiter");
    }
    test_case.sql = sql.str();
    ++index;

    while (index < lines.size() && !trim(lines[index]).empty()) {
      test_case.expected_output.push_back(decode_expected_line(lines[index]));
      ++index;
    }

    cases.push_back(std::move(test_case));
  }

  return cases;
}

void run_logic_file(const std::filesystem::path &path) {
  mattsql::DefaultSqlEngine engine;

  for (const auto &test_case : load_logic_cases(path)) {
    auto result = engine.Execute(test_case.sql);

    if (test_case.command == LogicCommand::StatementOk) {
      if (!mattsql::status_ok(result.status)) {
        fail_case(test_case, "expected success, got " + StatusName(result.status.code) +
                                 ": " + result.status.message);
      }
      if (!test_case.expected_output.empty()) {
        fail_case(test_case, "statement ok blocks must not include expected output");
      }
      continue;
    }

    if (test_case.command == LogicCommand::StatementError) {
      if (mattsql::status_ok(result.status)) {
        fail_case(test_case, "expected " + StatusName(*test_case.expected_error) +
                                 ", got success");
      }
      if (result.status.code != *test_case.expected_error) {
        fail_case(test_case, "expected " + StatusName(*test_case.expected_error) +
                                 ", got " + StatusName(result.status.code) + ": " +
                                 result.status.message);
      }
      if (!test_case.expected_output.empty()) {
        fail_case(test_case, "statement error blocks must not include expected output");
      }
      continue;
    }

    if (!mattsql::status_ok(result.status)) {
      fail_case(test_case, "expected query success, got " +
                               StatusName(result.status.code) + ": " +
                               result.status.message);
    }
    if (!result.value.has_value()) {
      fail_case(test_case, "query succeeded without a QueryResult");
    }

    auto actual_output = FormatQueryResult(*result.value);
    auto expected_output = test_case.expected_output;
    if (test_case.sort_rows) {
      SortQueryRows(actual_output);
      SortQueryRows(expected_output);
    }

    if (actual_output != expected_output) {
      fail_case(test_case, "query output mismatch\nexpected:\n" +
                               JoinLines(expected_output) + "actual:\n" +
                               JoinLines(actual_output));
    }
  }
}

} // namespace

void RunLogicDirectory(const std::filesystem::path &directory) {
  std::vector<std::filesystem::path> scripts;
  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    if (entry.is_regular_file() && entry.path().extension() == ".slt") {
      scripts.push_back(entry.path());
    }
  }

  std::sort(scripts.begin(), scripts.end());
  if (scripts.empty()) {
    throw std::runtime_error("no SQL logic files found in: " + directory.string());
  }

  for (const auto &script : scripts) {
    run_logic_file(script);
  }
}

} // namespace sql_logic
