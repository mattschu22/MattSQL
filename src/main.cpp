#include "mattsql/common/query_result.hpp"
#include "mattsql/common/result_utils.hpp"
#include "mattsql/common/value_utils.hpp"
#include "mattsql/sql_engine.hpp"

#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace {

struct Session {
  mattsql::DefaultSqlEngine engine;
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

[[nodiscard]] bool is_sql_file_path(std::string_view value) {
  if (value.size() < 4) {
    return false;
  }

  const auto extension = value.substr(value.size() - 4);
  return extension[0] == '.' && (extension[1] == 's' || extension[1] == 'S') &&
         (extension[2] == 'q' || extension[2] == 'Q') &&
         (extension[3] == 'l' || extension[3] == 'L');
}

[[nodiscard]] bool all_arguments_are_sql_files(int argc, char **argv) {
  for (int index = 1; index < argc; ++index) {
    if (!is_sql_file_path(argv[index])) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::string_view::size_type
find_statement_terminator(std::string_view buffer) {
  bool in_string = false;
  bool in_line_comment = false;
  bool in_block_comment = false;

  for (std::string_view::size_type index = 0; index < buffer.size(); ++index) {
    const auto character = buffer[index];
    const auto next = index + 1 < buffer.size() ? buffer[index + 1] : '\0';

    if (in_string) {
      if (character == '\'' && next == '\'') {
        ++index;
      } else if (character == '\'') {
        in_string = false;
      }
      continue;
    }

    if (in_line_comment) {
      if (character == '\n' || character == '\r') {
        in_line_comment = false;
      }
      continue;
    }

    if (in_block_comment) {
      if (character == '*' && next == '/') {
        in_block_comment = false;
        ++index;
      }
      continue;
    }

    if (character == '\'') {
      in_string = true;
      continue;
    }
    if (character == '-' && next == '-') {
      in_line_comment = true;
      ++index;
      continue;
    }
    if (character == '/' && next == '*') {
      in_block_comment = true;
      ++index;
      continue;
    }
    if (character == ';') {
      return index + 1;
    }
  }

  return std::string_view::npos;
}

// Parser currently accepts one statement at a time, so the CLI splits scripts on
// statement-terminating semicolons before parsing each statement independently.
[[nodiscard]] bool extract_statement(std::string &buffer, std::string &statement) {
  const auto end = find_statement_terminator(buffer);
  if (end == std::string_view::npos) {
    return false;
  }

  statement = buffer.substr(0, end);
  buffer.erase(0, end);
  return true;
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
      std::cout << mattsql::FormatValue(row[index]);
    }
    std::cout << '\n';
  }
}

[[nodiscard]] bool run_statement(Session &session, const std::string &sql) {
  const auto trimmed = trim(sql);
  if (trimmed.empty()) {
    return true;
  }

  try {
    auto result = session.engine.Execute(trimmed);
    if (!mattsql::status_ok(result.status)) {
      std::cerr << "error[" << mattsql::ErrorCodeName(result.status.code)
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

[[nodiscard]] bool run_sql_file(Session &session, std::string_view path) {
  std::ifstream input(std::string(path), std::ios::in | std::ios::binary);
  if (!input) {
    std::cerr << "error: unable to open SQL file: " << path << '\n';
    return false;
  }

  std::ostringstream sql;
  sql << input.rdbuf();
  if (input.bad()) {
    std::cerr << "error: unable to read SQL file: " << path << '\n';
    return false;
  }

  return run_sql_text(session, sql.str());
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
    if (all_arguments_are_sql_files(argc, argv)) {
      bool success = true;
      for (int index = 1; index < argc; ++index) {
        success = run_sql_file(session, argv[index]) && success;
      }
      return success ? 0 : 1;
    }

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
