#pragma once

#include "mattsql/common/query_result.hpp"
#include "mattsql/common/status.hpp"
#include "mattsql/common/value_utils.hpp"

#include <algorithm>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace sql_logic {

inline std::string StatusName(mattsql::ErrorCode code) {
  return std::string(mattsql::ErrorCodeName(code));
}

inline std::string JoinFields(const std::vector<std::string> &fields) {
  std::ostringstream output;
  for (std::size_t index = 0; index < fields.size(); ++index) {
    if (index != 0) {
      output << '\t';
    }
    output << fields[index];
  }
  return output.str();
}

inline std::vector<std::string> FormatQueryResult(const mattsql::QueryResult &result) {
  std::vector<std::string> output;
  output.push_back(JoinFields(result.columns));

  for (const auto &row : result.rows) {
    std::vector<std::string> fields;
    fields.reserve(row.size());
    for (const auto &value : row) {
      fields.push_back(mattsql::FormatValue(value));
    }
    output.push_back(JoinFields(fields));
  }

  return output;
}

inline std::string JoinLines(const std::vector<std::string> &lines) {
  std::ostringstream output;
  for (const auto &line : lines) {
    output << line << '\n';
  }
  return output.str();
}

inline void SortQueryRows(std::vector<std::string> &output) {
  if (output.size() > 1) {
    std::sort(output.begin() + 1, output.end());
  }
}

inline std::string SqlBool(bool value) { return value ? "true" : "false"; }

inline std::string QuoteSqlString(std::string_view value) {
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

} // namespace sql_logic
