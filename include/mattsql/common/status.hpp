#pragma once

#include <optional>
#include <string>
#include <string_view>

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

[[nodiscard]] inline std::string_view ErrorCodeName(ErrorCode code) {
  switch (code) {
  case ErrorCode::Ok:
    return "Ok";
  case ErrorCode::InvalidArgument:
    return "InvalidArgument";
  case ErrorCode::NotFound:
    return "NotFound";
  case ErrorCode::AlreadyExists:
    return "AlreadyExists";
  case ErrorCode::TypeMismatch:
    return "TypeMismatch";
  case ErrorCode::ParseError:
    return "ParseError";
  case ErrorCode::BindError:
    return "BindError";
  case ErrorCode::PlanError:
    return "PlanError";
  case ErrorCode::ExecutionError:
    return "ExecutionError";
  case ErrorCode::IoError:
    return "IoError";
  case ErrorCode::Corruption:
    return "Corruption";
  case ErrorCode::TransactionConflict:
    return "TransactionConflict";
  case ErrorCode::NotSupported:
    return "NotSupported";
  case ErrorCode::Internal:
    return "Internal";
  }

  return "Unknown";
}

[[nodiscard]] inline std::optional<ErrorCode>
ParseErrorCodeName(std::string_view name) {
  if (name == "Ok") {
    return ErrorCode::Ok;
  }
  if (name == "InvalidArgument") {
    return ErrorCode::InvalidArgument;
  }
  if (name == "NotFound") {
    return ErrorCode::NotFound;
  }
  if (name == "AlreadyExists") {
    return ErrorCode::AlreadyExists;
  }
  if (name == "TypeMismatch") {
    return ErrorCode::TypeMismatch;
  }
  if (name == "ParseError") {
    return ErrorCode::ParseError;
  }
  if (name == "BindError") {
    return ErrorCode::BindError;
  }
  if (name == "PlanError") {
    return ErrorCode::PlanError;
  }
  if (name == "ExecutionError") {
    return ErrorCode::ExecutionError;
  }
  if (name == "IoError") {
    return ErrorCode::IoError;
  }
  if (name == "Corruption") {
    return ErrorCode::Corruption;
  }
  if (name == "TransactionConflict") {
    return ErrorCode::TransactionConflict;
  }
  if (name == "NotSupported") {
    return ErrorCode::NotSupported;
  }
  if (name == "Internal") {
    return ErrorCode::Internal;
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
};

} // namespace mattsql
