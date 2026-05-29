#pragma once

#include "mattsql/common/types.hpp"

#include <string>
#include <vector>

namespace mattsql {

struct QueryResult {
  std::vector<std::string> columns;
  std::vector<std::vector<Value>> rows;
};

} // namespace mattsql
