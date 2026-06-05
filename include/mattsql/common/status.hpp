#pragma once

#include <array>
#include <cassert>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace mattsql {

enum class ErrorCode {
  Ok,
  InvalidArgument,
  NotFound,
  AlreadyExists,
  TypeMismatch,
  ParseError,
  BindError,
  PlanError,
  ExecutionError,
  IoError,
  Corruption,
  TransactionConflict,
  NotSupported,
  Internal
};

struct ErrorCodeNameEntry {
  ErrorCode code;
  std::string_view name;
};

inline constexpr std::array<ErrorCodeNameEntry, 14> kErrorCodeNames = {{
    {ErrorCode::Ok, "Ok"},
    {ErrorCode::InvalidArgument, "InvalidArgument"},
    {ErrorCode::NotFound, "NotFound"},
    {ErrorCode::AlreadyExists, "AlreadyExists"},
    {ErrorCode::TypeMismatch, "TypeMismatch"},
    {ErrorCode::ParseError, "ParseError"},
    {ErrorCode::BindError, "BindError"},
    {ErrorCode::PlanError, "PlanError"},
    {ErrorCode::ExecutionError, "ExecutionError"},
    {ErrorCode::IoError, "IoError"},
    {ErrorCode::Corruption, "Corruption"},
    {ErrorCode::TransactionConflict, "TransactionConflict"},
    {ErrorCode::NotSupported, "NotSupported"},
    {ErrorCode::Internal, "Internal"},
}};

[[nodiscard]] inline std::string_view ErrorCodeName(ErrorCode code) {
  for (const auto &entry : kErrorCodeNames) {
    if (entry.code == code) {
      return entry.name;
    }
  }

  return "Unknown";
}

[[nodiscard]] inline std::optional<ErrorCode>
ParseErrorCodeName(std::string_view name) {
  for (const auto &entry : kErrorCodeNames) {
    if (entry.name == name) {
      return entry.code;
    }
  }

  return std::nullopt;
}

struct Status {
  ErrorCode code = ErrorCode::Ok;
  std::string message;
};

template <typename T> struct Result {
  Status status;
  std::optional<T> value;

  [[nodiscard]] bool ok() const { return status.code == ErrorCode::Ok; }
  [[nodiscard]] bool has_value() const { return value.has_value(); }
  [[nodiscard]] explicit operator bool() const { return ok(); }

  [[nodiscard]] T &Value() & {
    assert(ok());
    assert(value.has_value());
    return *value;
  }

  [[nodiscard]] const T &Value() const & {
    assert(ok());
    assert(value.has_value());
    return *value;
  }

  [[nodiscard]] T &&TakeValue() && {
    assert(ok());
    assert(value.has_value());
    return std::move(*value);
  }
};

} // namespace mattsql
