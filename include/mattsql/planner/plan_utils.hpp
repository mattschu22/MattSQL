#pragma once

#include "mattsql/common/result_utils.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mattsql {

template <typename Plan>
[[nodiscard]] Status RequireChildCount(const Plan &plan, std::size_t expected,
                                       std::string_view operator_name, ErrorCode code) {
  if (plan.children.size() != expected) {
    return error_status(code,
                        std::string(operator_name) + " has the wrong number of inputs");
  }
  for (const auto &child : plan.children) {
    if (child == nullptr) {
      return error_status(code, std::string(operator_name) + " contains a null input");
    }
  }
  return ok_status();
}

template <typename Plan>
[[nodiscard]] Status RequireLeaf(const Plan &plan, std::string_view operator_name,
                                 ErrorCode code) {
  return RequireChildCount(plan, 0, operator_name, code);
}

} // namespace mattsql
