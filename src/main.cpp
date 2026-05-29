#include "mattsql/engine.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <sstream>
#include <string>
#include <variant>

namespace {

/// Reads the entire standard input stream into a SQL string.
[[nodiscard]] std::string read_stdin() {
  std::ostringstream buffer;
  buffer << std::cin.rdbuf();
  return buffer.str();
}

/// Converts a typed SQL value into the CLI's tabular text format.
[[nodiscard]] std::string format_value(const mattsql::Value& value) {
  if (std::holds_alternative<mattsql::NullValue>(value)) {
    return "NULL";
  }
  if (const auto* integer = std::get_if<std::int64_t>(&value)) {
    return std::to_string(*integer);
  }
  if (const auto* boolean = std::get_if<bool>(&value)) {
    return *boolean ? "true" : "false";
  }
  return std::get<std::string>(value);
}

/// Prints a query result as tab-separated columns and rows.
void print_result(const mattsql::QueryResult& result) {
  if (result.columns.empty()) {
    return;
  }

  for (std::size_t index = 0; index < result.columns.size(); ++index) {
    if (index != 0) {
      std::cout << '\t';
    }
    std::cout << result.columns[index];
  }
  std::cout << '\n';

  for (const auto& row : result.rows) {
    for (std::size_t index = 0; index < row.size(); ++index) {
      if (index != 0) {
        std::cout << '\t';
      }
      std::cout << format_value(row[index]);
    }
    std::cout << '\n';
  }
}

} // namespace

/// Runs the MattSQL command-line entry point.
int main(int argc, char** argv) {
  std::string sql;

  if (argc > 1) {
    for (int index = 1; index < argc; ++index) {
      if (!sql.empty()) {
        sql.push_back(' ');
      }
      sql.append(argv[index]);
    }
  } else {
    sql = read_stdin();
  }

  try {
    mattsql::Engine engine;
    print_result(engine.execute(sql));
  } catch (const std::exception& error) {
    std::cerr << "mattsql: " << error.what() << '\n';
    return 1;
  }

  return 0;
}
