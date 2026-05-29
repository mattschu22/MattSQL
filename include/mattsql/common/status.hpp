#pragma once

#include <optional>
#include <string>

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

struct Status {
    ErrorCode code = ErrorCode::Ok;
    std::string message;
};

template <typename T>
struct Result {
    Status status;
    std::optional<T> value;
};

} // namespace mattsql
