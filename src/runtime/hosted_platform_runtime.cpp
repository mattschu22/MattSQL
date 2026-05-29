#include "mattsql/runtime/hosted_platform_runtime.hpp"

#include "mattsql/common/result_utils.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <limits>
#include <new>
#include <string_view>
#include <thread>
#include <utility>

namespace mattsql {
namespace {

[[nodiscard]] std::string_view log_level_name(LogLevel level) {
  switch (level) {
  case LogLevel::Trace:
    return "trace";
  case LogLevel::Debug:
    return "debug";
  case LogLevel::Info:
    return "info";
  case LogLevel::Warn:
    return "warn";
  case LogLevel::Error:
    return "error";
  case LogLevel::Fatal:
    return "fatal";
  }

  return "unknown";
}

} // namespace

struct HostedPlatformRuntime::Impl {
  IoRequestId next_io_request_id = 1;
  RuntimeTaskId next_task_id = 1;
  std::deque<IoCompletion> completions;
};

HostedPlatformRuntime::HostedPlatformRuntime() : impl_(std::make_unique<Impl>()) {}

HostedPlatformRuntime::~HostedPlatformRuntime() = default;

HostedPlatformRuntime::HostedPlatformRuntime(HostedPlatformRuntime &&) noexcept =
    default;

HostedPlatformRuntime &
HostedPlatformRuntime::operator=(HostedPlatformRuntime &&) noexcept = default;

Result<RuntimePageAllocation>
HostedPlatformRuntime::AllocatePages(std::size_t page_count) {
  if (page_count == 0) {
    return error_result<RuntimePageAllocation>(ErrorCode::InvalidArgument,
                                               "page count must be positive");
  }
  if (page_count > std::numeric_limits<std::size_t>::max() / kDefaultPageSize) {
    return error_result<RuntimePageAllocation>(ErrorCode::InvalidArgument,
                                               "page allocation is too large");
  }

  const auto byte_count = page_count * kDefaultPageSize;
  auto *data = new (std::nothrow) std::byte[byte_count]();
  if (data == nullptr) {
    return error_result<RuntimePageAllocation>(ErrorCode::Internal,
                                               "host page allocation failed");
  }

  RuntimePageAllocation allocation;
  allocation.data = data;
  allocation.page_count = page_count;
  allocation.page_size = kDefaultPageSize;
  allocation.physical_address = reinterpret_cast<std::uintptr_t>(data);
  return ok_result(allocation);
}

Status HostedPlatformRuntime::FreePages(const RuntimePageAllocation &allocation) {
  if (allocation.data == nullptr) {
    return error_status(ErrorCode::InvalidArgument, "allocation data is null");
  }

  delete[] static_cast<std::byte *>(allocation.data);
  return ok_status();
}

Result<IoRequestId> HostedPlatformRuntime::SubmitIo(const IoRequest &request) {
  auto id = request.id;
  if (id == 0) {
    id = impl_->next_io_request_id++;
  }

  IoCompletion completion;
  completion.id = id;
  completion.status =
      error_status(ErrorCode::NotSupported, "hosted runtime has no block device");
  completion.bytes_transferred = 0;
  impl_->completions.push_back(std::move(completion));
  return ok_result(id);
}

Result<IoCompletion> HostedPlatformRuntime::PollIoCompletion() {
  if (impl_->completions.empty()) {
    return error_result<IoCompletion>(ErrorCode::NotFound,
                                      "no I/O completions are ready");
  }

  auto completion = std::move(impl_->completions.front());
  impl_->completions.pop_front();
  return ok_result(std::move(completion));
}

Result<RuntimeTaskId>
HostedPlatformRuntime::SpawnTask(const TaskDescriptor &descriptor) {
  (void)descriptor;
  return ok_result(impl_->next_task_id++);
}

Status HostedPlatformRuntime::Yield() {
  std::this_thread::yield();
  return ok_status();
}

std::uint64_t HostedPlatformRuntime::MonotonicNanos() const {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

void HostedPlatformRuntime::Log(LogLevel level, std::string_view message) {
  std::cerr << '[' << log_level_name(level) << "] " << message << '\n';
}

[[noreturn]] void HostedPlatformRuntime::Panic(std::string_view message) {
  Log(LogLevel::Fatal, message);
  std::abort();
}

} // namespace mattsql
