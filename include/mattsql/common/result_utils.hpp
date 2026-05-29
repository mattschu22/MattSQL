#pragma once

#include "mattsql/common/status.hpp"

#include <string>
#include <utility>

namespace mattsql {

/// Returns true when a status represents success.
[[nodiscard]] inline bool status_ok(const Status &status) {
  return status.code == ErrorCode::Ok;
}

/// Creates a success status.
[[nodiscard]] inline Status ok_status() { return {}; }

/// Creates a failure status with the supplied error code and message.
[[nodiscard]] inline Status error_status(ErrorCode code, std::string message) {
  return Status{code, std::move(message)};
}

/// Creates a successful Result containing the supplied value.
template <typename T> [[nodiscard]] Result<T> ok_result(T value) {
  Result<T> result;
  result.value.emplace(std::move(value));
  return result;
}

/// Creates a failed Result from an existing status.
template <typename T> [[nodiscard]] Result<T> error_result(Status status) {
  Result<T> result;
  result.status = std::move(status);
  return result;
}

/// Creates a failed Result from an error code and message.
template <typename T>
[[nodiscard]] Result<T> error_result(ErrorCode code, std::string message) {
  return error_result<T>(error_status(code, std::move(message)));
}

} // namespace mattsql
