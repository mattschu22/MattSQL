#pragma once

#include "mattsql/common/result_utils.hpp"
#include "mattsql/common/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace mattsql {

[[nodiscard]] inline bool IsNull(const Value &value) {
  return std::holds_alternative<NullValue>(value);
}

[[nodiscard]] inline SqlType ValueTypeOf(const Value &value) {
  if (std::holds_alternative<std::int64_t>(value)) {
    return SqlType::Integer;
  }
  if (std::holds_alternative<std::string>(value)) {
    return SqlType::Text;
  }
  if (std::holds_alternative<bool>(value)) {
    return SqlType::Boolean;
  }
  return SqlType::Null;
}

[[nodiscard]] inline bool ValueMatchesType(const Value &value, SqlType type) {
  return ValueTypeOf(value) == type;
}

[[nodiscard]] inline std::optional<std::int64_t> IntegralValue(const Value &value) {
  if (const auto *integer = std::get_if<std::int64_t>(&value)) {
    return *integer;
  }
  if (const auto *boolean = std::get_if<bool>(&value)) {
    return *boolean ? 1 : 0;
  }
  return std::nullopt;
}

[[nodiscard]] inline Result<std::int64_t> RequireInteger(const Value &value,
                                                         std::string_view context) {
  if (const auto *integer = std::get_if<std::int64_t>(&value)) {
    return ok_result(*integer);
  }

  return error_result<std::int64_t>(
      ErrorCode::TypeMismatch, std::string(context) + " requires INTEGER operands");
}

[[nodiscard]] inline Result<bool> RequireBoolean(const Value &value,
                                                 std::string_view context) {
  if (const auto *boolean = std::get_if<bool>(&value)) {
    return ok_result(*boolean);
  }

  return error_result<bool>(ErrorCode::TypeMismatch,
                            std::string(context) + " requires BOOLEAN operands");
}

[[nodiscard]] inline Result<int> CompareValues(const Value &left, const Value &right) {
  if (IsNull(left) || IsNull(right)) {
    return error_result<int>(ErrorCode::ExecutionError, "cannot compare NULL values");
  }

  const auto left_integral = IntegralValue(left);
  const auto right_integral = IntegralValue(right);
  if (left_integral.has_value() && right_integral.has_value()) {
    if (*left_integral < *right_integral) {
      return ok_result(-1);
    }
    if (*right_integral < *left_integral) {
      return ok_result(1);
    }
    return ok_result(0);
  }

  const auto *left_string = std::get_if<std::string>(&left);
  const auto *right_string = std::get_if<std::string>(&right);
  if (left_string != nullptr && right_string != nullptr) {
    if (*left_string < *right_string) {
      return ok_result(-1);
    }
    if (*right_string < *left_string) {
      return ok_result(1);
    }
    return ok_result(0);
  }

  return error_result<int>(ErrorCode::TypeMismatch,
                           "cannot compare values with different types");
}

[[nodiscard]] inline std::string FormatValue(const Value &value) {
  if (IsNull(value)) {
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

} // namespace mattsql
