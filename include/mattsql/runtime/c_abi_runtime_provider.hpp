#pragma once

#include "mattsql/abi/runtime.h"
#include "mattsql/runtime/platform.hpp"

#include <string>
#include <vector>

namespace mattsql {

class CAbiRuntimeProvider final {
public:
  explicit CAbiRuntimeProvider(PlatformRuntime &runtime);

  CAbiRuntimeProvider(const CAbiRuntimeProvider &) = delete;
  CAbiRuntimeProvider &operator=(const CAbiRuntimeProvider &) = delete;
  CAbiRuntimeProvider(CAbiRuntimeProvider &&) = delete;
  CAbiRuntimeProvider &operator=(CAbiRuntimeProvider &&) = delete;

  /// Returns a C ABI table backed by the provider's PlatformRuntime.
  [[nodiscard]] mattsql_abi_runtime_v1 RuntimeTable() const;

  /// Returns the runtime backing this ABI provider.
  [[nodiscard]] PlatformRuntime &GetRuntime() const;

private:
  static mattsql_abi_status GetCapabilities(
      void *context, mattsql_abi_runtime_capabilities *out_capabilities);
  static mattsql_abi_status AllocatePages(
      void *context, std::uint64_t page_count, std::uint64_t alignment,
      mattsql_abi_runtime_memory_flags flags,
      mattsql_abi_page_allocation *out_allocation);
  static mattsql_abi_status FreePages(
      void *context, const mattsql_abi_page_allocation *allocation);
  static mattsql_abi_status SubmitIoBatch(
      void *context, const mattsql_abi_io_request *requests,
      std::uint64_t request_count,
      mattsql_abi_io_submission_result *out_submission);
  static mattsql_abi_status PollIoCompletions(
      void *context, mattsql_abi_io_completion *completions,
      std::uint64_t max_completion_count, std::uint64_t *out_completion_count);
  static mattsql_abi_status Yield(void *context);
  static mattsql_abi_status MonotonicNanos(void *context,
                                           std::uint64_t *out_nanos);
  static void Log(void *context, mattsql_abi_log_level level,
                  const char *message, std::uint64_t message_length);
  [[noreturn]] static void Panic(void *context, const char *message,
                                 std::uint64_t message_length);

  [[nodiscard]] mattsql_abi_status StoreStatus(const Status &status) const;
  [[nodiscard]] mattsql_abi_status StoreError(ErrorCode code,
                                              std::string_view message) const;

  PlatformRuntime *runtime_ = nullptr;
  mattsql_abi_runtime_v1 runtime_table_{};
  mutable std::string status_message_;
  mutable std::vector<std::string> completion_status_messages_;
};

} // namespace mattsql
