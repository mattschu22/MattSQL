#include "sql_logic_support.hpp"

#include "mattsql/common/result_utils.hpp"
#include "mattsql/common/value_utils.hpp"
#include "mattsql/sql_engine.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
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

std::string status_name(mattsql::ErrorCode code) {
  return std::string(mattsql::ErrorCodeName(code));
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

std::string join_fields(const std::vector<std::string> &fields) {
  std::ostringstream output;
  for (std::size_t index = 0; index < fields.size(); ++index) {
    if (index != 0) {
      output << '\t';
    }
    output << fields[index];
  }
  return output.str();
}

std::vector<std::string> format_query_result(const mattsql::QueryResult &result) {
  std::vector<std::string> output;
  output.push_back(join_fields(result.columns));

  for (const auto &row : result.rows) {
    std::vector<std::string> fields;
    fields.reserve(row.size());
    for (const auto &value : row) {
      fields.push_back(mattsql::FormatValue(value));
    }
    output.push_back(join_fields(fields));
  }

  return output;
}

std::string join_lines(const std::vector<std::string> &lines) {
  std::ostringstream output;
  for (const auto &line : lines) {
    output << line << '\n';
  }
  return output.str();
}

void sort_query_rows(std::vector<std::string> &output) {
  if (output.size() > 1) {
    std::sort(output.begin() + 1, output.end());
  }
}

void run_logic_file(const std::filesystem::path &path) {
  mattsql::DefaultSqlEngine engine;

  for (const auto &test_case : load_logic_cases(path)) {
    auto result = engine.Execute(test_case.sql);

    if (test_case.command == LogicCommand::StatementOk) {
      if (!mattsql::status_ok(result.status)) {
        fail_case(test_case, "expected success, got " +
                                 status_name(result.status.code) + ": " +
                                 result.status.message);
      }
      if (!test_case.expected_output.empty()) {
        fail_case(test_case, "statement ok blocks must not include expected output");
      }
      continue;
    }

    if (test_case.command == LogicCommand::StatementError) {
      if (mattsql::status_ok(result.status)) {
        fail_case(test_case, "expected " + status_name(*test_case.expected_error) +
                                 ", got success");
      }
      if (result.status.code != *test_case.expected_error) {
        fail_case(test_case, "expected " + status_name(*test_case.expected_error) +
                                 ", got " + status_name(result.status.code) + ": " +
                                 result.status.message);
      }
      if (!test_case.expected_output.empty()) {
        fail_case(test_case, "statement error blocks must not include expected output");
      }
      continue;
    }

    if (!mattsql::status_ok(result.status)) {
      fail_case(test_case, "expected query success, got " +
                               status_name(result.status.code) + ": " +
                               result.status.message);
    }
    if (!result.value.has_value()) {
      fail_case(test_case, "query succeeded without a QueryResult");
    }

    auto actual_output = format_query_result(*result.value);
    auto expected_output = test_case.expected_output;
    if (test_case.sort_rows) {
      sort_query_rows(actual_output);
      sort_query_rows(expected_output);
    }

    if (actual_output != expected_output) {
      fail_case(test_case, "query output mismatch\nexpected:\n" +
                               join_lines(expected_output) + "actual:\n" +
                               join_lines(actual_output));
    }
  }
}

class FuzzRng {
public:
  explicit FuzzRng(std::uint64_t seed) : state_(seed) {}

  std::uint32_t NextU32() {
    state_ = state_ * std::uint64_t{6364136223846793005} +
             std::uint64_t{1442695040888963407};
    return static_cast<std::uint32_t>(state_ >> 32U);
  }

  std::size_t NextIndex(std::size_t bound) {
    return static_cast<std::size_t>(NextU32() % static_cast<std::uint32_t>(bound));
  }

  int Between(int minimum, int maximum) {
    const auto span = static_cast<std::uint32_t>(maximum - minimum + 1);
    return minimum + static_cast<int>(NextU32() % span);
  }

  bool Coin() { return (NextU32() & 1U) == 1U; }

private:
  std::uint64_t state_;
};

struct FuzzRow {
  int id = 0;
  std::string name;
  bool active = false;
  int score = 0;
};

struct FuzzPredicate {
  int kind = 0;
  int first = 0;
  int second = 0;
  bool flag = false;
  std::string name;
  std::string sql;
};

struct FuzzQuery {
  std::string sql;
  mattsql::QueryResult expected;
  bool sort_rows = false;
};

std::string sql_bool(bool value) { return value ? "true" : "false"; }

std::string quote_sql_string(std::string_view value) {
  std::string output = "'";
  for (const char character : value) {
    if (character == '\'') {
      output += "''";
    } else {
      output.push_back(character);
    }
  }
  output.push_back('\'');
  return output;
}

std::string make_fuzz_name(FuzzRng &rng, std::size_t row_index) {
  constexpr std::string_view bases[] = {"Ada",    "Grace",  "Lin",
                                        "O'Neil", "Turing", "Kay"};
  constexpr std::size_t base_count = sizeof(bases) / sizeof(bases[0]);

  std::string name(bases[rng.NextIndex(base_count)]);
  name += '_';
  name += std::to_string(row_index);
  name += '_';

  const auto suffix_length = rng.Between(8, 36);
  for (int index = 0; index < suffix_length; ++index) {
    name.push_back(static_cast<char>('a' + rng.Between(0, 25)));
  }

  return name;
}

bool matches_predicate(const FuzzRow &row, const FuzzPredicate &predicate) {
  switch (predicate.kind) {
  case 0:
    return row.score >= predicate.first;
  case 1:
    return row.id <= predicate.first;
  case 2:
    return row.active == predicate.flag;
  case 3:
    return row.name == predicate.name;
  case 4:
    return row.active == predicate.flag && row.score >= predicate.first;
  case 5:
    return row.active == predicate.flag || row.id == predicate.first;
  case 6:
    return (row.score >= predicate.first && row.score <= predicate.second) ||
           row.name == predicate.name;
  case 7:
    return !row.active;
  case 8:
    return row.id != predicate.first;
  default:
    return false;
  }
}

FuzzPredicate make_predicate(FuzzRng &rng, const std::vector<FuzzRow> &rows) {
  FuzzPredicate predicate;
  predicate.kind = static_cast<int>(rng.NextIndex(9));
  const auto &sample = rows[rng.NextIndex(rows.size())];

  switch (predicate.kind) {
  case 0:
    predicate.first = rng.Between(-55, 55);
    predicate.sql = "score >= " + std::to_string(predicate.first);
    break;
  case 1:
    predicate.first = rng.Between(1, static_cast<int>(rows.size()));
    predicate.sql = "id <= " + std::to_string(predicate.first);
    break;
  case 2:
    predicate.flag = rng.Coin();
    predicate.sql = "active = " + sql_bool(predicate.flag);
    break;
  case 3:
    predicate.name = sample.name;
    predicate.sql = "name = " + quote_sql_string(predicate.name);
    break;
  case 4:
    predicate.flag = rng.Coin();
    predicate.first = rng.Between(-55, 55);
    predicate.sql = "active = " + sql_bool(predicate.flag) +
                    " AND score >= " + std::to_string(predicate.first);
    break;
  case 5:
    predicate.flag = rng.Coin();
    predicate.first = sample.id;
    predicate.sql = "active = " + sql_bool(predicate.flag) +
                    " OR id = " + std::to_string(predicate.first);
    break;
  case 6: {
    const auto left = rng.Between(-55, 55);
    const auto right = rng.Between(-55, 55);
    predicate.first = std::min(left, right);
    predicate.second = std::max(left, right);
    predicate.name = sample.name;
    predicate.sql = "(score >= " + std::to_string(predicate.first) +
                    " AND score <= " + std::to_string(predicate.second) +
                    ") OR name = " + quote_sql_string(predicate.name);
    break;
  }
  case 7:
    predicate.sql = "NOT active";
    break;
  case 8:
    predicate.first = sample.id;
    predicate.sql = "id <> " + std::to_string(predicate.first);
    break;
  default:
    predicate.sql = "true";
    break;
  }

  return predicate;
}

std::string add_integer_expression(std::string_view expression, int delta) {
  if (delta < 0) {
    return std::string(expression) + " - " + std::to_string(-delta);
  }
  return std::string(expression) + " + " + std::to_string(delta);
}

void add_expected_row(mattsql::QueryResult &result, const FuzzRow &row, int query_kind,
                      int delta) {
  switch (query_kind) {
  case 0:
    result.rows.push_back(
        {std::int64_t{row.id}, row.name, row.active, std::int64_t{row.score}});
    break;
  case 1:
    result.rows.push_back({std::int64_t{row.id}, std::int64_t{row.score + delta}});
    break;
  case 2:
    result.rows.push_back({row.name, row.active});
    break;
  case 3:
    result.rows.push_back(
        {std::int64_t{row.id}, std::int64_t{row.score * 2 + row.id}, row.active});
    break;
  case 4:
    result.rows.push_back({std::int64_t{row.id}, row.name});
    break;
  default:
    break;
  }
}

FuzzQuery make_table_query(FuzzRng &rng, const std::vector<FuzzRow> &rows) {
  const auto predicate = make_predicate(rng, rows);
  const auto query_kind = static_cast<int>(rng.NextIndex(5));
  const auto delta = rng.Between(-12, 12);

  FuzzQuery query;
  query.sort_rows = true;
  switch (query_kind) {
  case 0:
    query.sql = "SELECT * FROM fuzz WHERE " + predicate.sql + ';';
    query.expected.columns = {"id", "name", "active", "score"};
    break;
  case 1:
    query.sql = "SELECT id AS row_id, " + add_integer_expression("score", delta) +
                " AS shifted FROM fuzz WHERE " + predicate.sql + ';';
    query.expected.columns = {"row_id", "shifted"};
    break;
  case 2:
    query.sql = "SELECT name, active FROM fuzz WHERE " + predicate.sql + ';';
    query.expected.columns = {"name", "active"};
    break;
  case 3:
    query.sql = "SELECT id AS row_id, score * 2 + id AS metric, active = true AS "
                "is_active FROM fuzz WHERE " +
                predicate.sql + ';';
    query.expected.columns = {"row_id", "metric", "is_active"};
    break;
  case 4:
    query.sql = "SELECT fuzz.id AS id, fuzz.name AS name FROM fuzz WHERE " +
                predicate.sql + ';';
    query.expected.columns = {"id", "name"};
    break;
  default:
    break;
  }

  for (const auto &row : rows) {
    if (matches_predicate(row, predicate)) {
      add_expected_row(query.expected, row, query_kind, delta);
    }
  }

  return query;
}

FuzzQuery make_scalar_query(FuzzRng &rng) {
  const auto left = rng.Between(-20, 20);
  const auto right = rng.Between(-20, 20);
  const auto multiplier = rng.Between(-5, 5);

  FuzzQuery query;
  query.sql = "SELECT (" + std::to_string(left) + " + " + std::to_string(right) +
              ") * " + std::to_string(multiplier) + " AS value, NOT false AS truth;";
  query.expected.columns = {"value", "truth"};
  query.expected.rows.push_back({std::int64_t{(left + right) * multiplier}, true});
  return query;
}

[[noreturn]] void fail_fuzz(std::uint64_t seed, const std::string &sql,
                            const std::string &message) {
  std::ostringstream output;
  output << "SQL fuzz seed " << seed << " failed: " << message << "\nSQL:\n" << sql;
  throw std::runtime_error(output.str());
}

void execute_ok(mattsql::DefaultSqlEngine &engine, std::uint64_t seed,
                const std::string &sql) {
  const auto result = engine.Execute(sql);
  if (!mattsql::status_ok(result.status)) {
    fail_fuzz(seed, sql,
              "expected success, got " + status_name(result.status.code) + ": " +
                  result.status.message);
  }
}

void expect_query(mattsql::DefaultSqlEngine &engine, std::uint64_t seed,
                  const FuzzQuery &query) {
  const auto result = engine.Execute(query.sql);
  if (!mattsql::status_ok(result.status)) {
    fail_fuzz(seed, query.sql,
              "expected query success, got " + status_name(result.status.code) + ": " +
                  result.status.message);
  }
  if (!result.value.has_value()) {
    fail_fuzz(seed, query.sql, "query succeeded without a QueryResult");
  }

  auto actual_output = format_query_result(*result.value);
  auto expected_output = format_query_result(query.expected);
  if (query.sort_rows) {
    sort_query_rows(actual_output);
    sort_query_rows(expected_output);
  }

  if (actual_output != expected_output) {
    fail_fuzz(seed, query.sql,
              "query output mismatch\nexpected:\n" + join_lines(expected_output) +
                  "actual:\n" + join_lines(actual_output));
  }
}

void run_sql_logic_fuzzer_seed(std::uint64_t seed) {
  constexpr std::size_t row_count = 96;
  constexpr std::size_t query_count = 32;

  FuzzRng rng(seed);
  mattsql::DefaultSqlEngine engine;
  execute_ok(engine, seed,
             "CREATE TABLE fuzz (id INT, name TEXT, active BOOL, score INT);");

  std::vector<FuzzRow> rows;
  rows.reserve(row_count);
  for (std::size_t row_index = 0; row_index < row_count; ++row_index) {
    FuzzRow row;
    row.id = static_cast<int>(row_index + 1U);
    row.name = make_fuzz_name(rng, row_index);
    row.active = rng.Coin();
    row.score = rng.Between(-50, 50);

    const auto insert_sql = "INSERT INTO fuzz VALUES (" + std::to_string(row.id) +
                            ", " + quote_sql_string(row.name) + ", " +
                            sql_bool(row.active) + ", " + std::to_string(row.score) +
                            ");";
    execute_ok(engine, seed, insert_sql);
    rows.push_back(std::move(row));
  }

  for (std::size_t query_index = 0; query_index < query_count; ++query_index) {
    expect_query(engine, seed, make_table_query(rng, rows));
    expect_query(engine, seed, make_scalar_query(rng));
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

void RunDeterministicFuzzerSeeds(std::uint64_t first_seed, std::size_t count) {
  for (std::uint64_t seed_index = 0; seed_index < count; ++seed_index) {
    run_sql_logic_fuzzer_seed(first_seed + seed_index);
  }
}

} // namespace sql_logic
