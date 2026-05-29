#pragma once

#include "mattsql/runtime/platform.hpp"

#include <deque>
#include <memory>

namespace mattsql {

class HostedPlatformRuntime final : public PlatformRuntime {
public:
  HostedPlatformRuntime();
  ~HostedPlatformRuntime() override;

  HostedPlatformRuntime(const HostedPlatformRuntime &) = delete;
  HostedPlatformRuntime &operator=(const HostedPlatformRuntime &) = delete;

  HostedPlatformRuntime(HostedPlatformRuntime &&) noexcept;
  HostedPlatformRuntime &operator=(HostedPlatformRuntime &&) noexcept;

  /// Allocates host heap memory through the runtime boundary.
  Result<RuntimePageAllocation> AllocatePages(std::size_t page_count) override;

  /// Frees host heap memory previously allocated by AllocatePages.
  Status FreePages(const RuntimePageAllocation &allocation) override;

  /// Records a completed unsupported I/O request for deterministic hosted tests.
  Result<IoRequestId> SubmitIo(const IoRequest &request) override;

  /// Returns the next completed hosted I/O request.
  Result<IoCompletion> PollIoCompletion() override;

  /// Allocates a task identifier without starting an OS thread.
  Result<RuntimeTaskId> SpawnTask(const TaskDescriptor &descriptor) override;

  /// Yields to the host scheduler.
  Status Yield() override;

  /// Returns host monotonic time in nanoseconds.
  std::uint64_t MonotonicNanos() const override;

  /// Emits a diagnostic message to stderr.
  void Log(LogLevel level, std::string_view message) override;

  /// Aborts the hosted process.
  [[noreturn]] void Panic(std::string_view message) override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace mattsql
