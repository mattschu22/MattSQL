#pragma once

#include "mattsql/runtime/platform.hpp"

#include <deque>
#include <memory>

namespace mattsql {

class BlockDevice;

class HostedPlatformRuntime final : public PlatformRuntime {
public:
  using PlatformRuntime::AllocatePages;

  HostedPlatformRuntime();
  explicit HostedPlatformRuntime(BlockDevice &block_device);
  ~HostedPlatformRuntime() override;

  HostedPlatformRuntime(const HostedPlatformRuntime &) = delete;
  HostedPlatformRuntime &operator=(const HostedPlatformRuntime &) = delete;

  HostedPlatformRuntime(HostedPlatformRuntime &&) noexcept;
  HostedPlatformRuntime &operator=(HostedPlatformRuntime &&) noexcept;

  /// Returns hosted runtime limits and optional attached block-device geometry.
  RuntimeCapabilities GetCapabilities() const override;

  /// Allocates aligned host heap memory through the runtime boundary.
  Result<RuntimePageAllocation>
  AllocatePages(std::size_t page_count, std::size_t alignment,
                RuntimeMemoryFlags flags) override;

  /// Frees host heap memory previously allocated by AllocatePages.
  Status FreePages(const RuntimePageAllocation &allocation) override;

  /// Executes or records a batch-shaped hosted I/O submission.
  Result<IoSubmissionResult>
  SubmitIoBatch(std::span<const IoRequest> requests) override;

  /// Returns up to the requested number of completed hosted I/O requests.
  Result<std::size_t> PollIoCompletions(std::span<IoCompletion> completions) override;

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
