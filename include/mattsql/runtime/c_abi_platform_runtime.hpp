#pragma once

#include "mattsql/abi/runtime.h"
#include "mattsql/runtime/platform.hpp"

namespace mattsql {

class CAbiPlatformRuntime final : public PlatformRuntime {
public:
  using PlatformRuntime::AllocatePages;

  explicit CAbiPlatformRuntime(mattsql_abi_runtime_v1 runtime);
  ~CAbiPlatformRuntime() override;

  CAbiPlatformRuntime(const CAbiPlatformRuntime &) = delete;
  CAbiPlatformRuntime &operator=(const CAbiPlatformRuntime &) = delete;

  CAbiPlatformRuntime(CAbiPlatformRuntime &&) noexcept;
  CAbiPlatformRuntime &operator=(CAbiPlatformRuntime &&) noexcept;

  /// Validates the version and required function pointers in the ABI table.
  [[nodiscard]] Status Validate() const;

  /// Returns runtime capabilities reported by the ABI backend.
  RuntimeCapabilities GetCapabilities() const override;

  /// Allocates runtime-managed page spans through the ABI backend.
  Result<RuntimePageAllocation>
  AllocatePages(std::size_t page_count, std::size_t alignment,
                RuntimeMemoryFlags flags) override;

  /// Releases pages previously returned by AllocatePages.
  Status FreePages(const RuntimePageAllocation &allocation) override;

  /// Submits one or more I/O requests through the ABI backend.
  Result<IoSubmissionResult>
  SubmitIoBatch(std::span<const IoRequest> requests) override;

  /// Polls for completed I/O requests through the ABI backend.
  Result<std::size_t> PollIoCompletions(std::span<IoCompletion> completions) override;

  /// Task creation is intentionally outside the MVP C ABI.
  Result<RuntimeTaskId> SpawnTask(const TaskDescriptor &descriptor) override;

  /// Yields through the ABI backend when a yield callback is installed.
  Status Yield() override;

  /// Returns monotonic runtime time from the ABI backend.
  std::uint64_t MonotonicNanos() const override;

  /// Emits a diagnostic log message through the ABI backend.
  void Log(LogLevel level, std::string_view message) override;

  /// Aborts the runtime through the ABI backend.
  [[noreturn]] void Panic(std::string_view message) override;

private:
  mattsql_abi_runtime_v1 runtime_;
};

} // namespace mattsql
