#include "mattsql/runtime/hosted_platform_runtime.hpp"

#include "mattsql/common/result_utils.hpp"
#include "mattsql/common/trace.hpp"
#include "mattsql/storage/block_device.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <limits>
#include <cstring>
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
  BlockDevice *block_device = nullptr;
  IoRequestId next_io_request_id = 1;
  RuntimeTaskId next_task_id = 1;
  std::deque<IoCompletion> completions;
};

namespace {

inline constexpr std::size_t kHostedMaxIoRequestSize = std::size_t{1} << 20U;
inline constexpr std::size_t kHostedMaxIoBatchSize = 64;
inline constexpr std::size_t kHostedMaxOutstandingIo = 1024;
inline constexpr RuntimeMemoryFlags kHostedKnownMemoryFlags =
    kRuntimeMemoryZeroed | kRuntimeMemoryDma;
inline constexpr IoRequestFlags kHostedKnownIoRequestFlags =
    kIoRequestBarrierBefore | kIoRequestBarrierAfter | kIoRequestForceUnitAccess;

[[nodiscard]] bool is_power_of_two(std::size_t value) {
  return value != 0 && (value & (value - 1U)) == 0;
}

[[nodiscard]] std::size_t effective_request_length(const IoRequest &request) {
  if (request.length != 0) {
    return request.length;
  }
  return request.buffer.bytes.size();
}

[[nodiscard]] Status validate_buffered_io_request(const IoRequest &request,
                                                  std::size_t length) {
  if (length == 0) {
    return error_status(ErrorCode::InvalidArgument,
                        "buffered I/O requests must be non-empty");
  }
  if (length > request.buffer.bytes.size()) {
    return error_status(ErrorCode::InvalidArgument,
                        "I/O request length exceeds its buffer");
  }
  if (length > kHostedMaxIoRequestSize) {
    return error_status(ErrorCode::InvalidArgument,
                        "I/O request exceeds hosted maximum request size");
  }
  return ok_status();
}

} // namespace

HostedPlatformRuntime::HostedPlatformRuntime() : impl_(std::make_unique<Impl>()) {}

HostedPlatformRuntime::HostedPlatformRuntime(BlockDevice &block_device)
    : impl_(std::make_unique<Impl>()) {
  impl_->block_device = &block_device;
}

HostedPlatformRuntime::~HostedPlatformRuntime() = default;

HostedPlatformRuntime::HostedPlatformRuntime(HostedPlatformRuntime &&) noexcept =
    default;

HostedPlatformRuntime &
HostedPlatformRuntime::operator=(HostedPlatformRuntime &&) noexcept = default;

RuntimeCapabilities HostedPlatformRuntime::GetCapabilities() const {
  RuntimeCapabilities capabilities;
  capabilities.page_size = kDefaultPageSize;
  capabilities.block_size =
      impl_->block_device == nullptr ? 0 : impl_->block_device->BlockSize();
  capabilities.max_io_request_size = kHostedMaxIoRequestSize;
  capabilities.max_io_batch_size = kHostedMaxIoBatchSize;
  capabilities.max_outstanding_io = kHostedMaxOutstandingIo;
  capabilities.supports_async_io = false;
  capabilities.supports_flush = impl_->block_device != nullptr;
  capabilities.supports_barriers = impl_->block_device != nullptr;
  capabilities.supports_physical_addresses = false;
  capabilities.supports_dma_memory = false;
  return capabilities;
}

Result<RuntimePageAllocation>
HostedPlatformRuntime::AllocatePages(std::size_t page_count,
                                     std::size_t alignment,
                                     RuntimeMemoryFlags flags) {
  ScopedTrace trace("mattsql::HostedPlatformRuntime::AllocatePages",
                    "function.runtime");
  if (page_count == 0) {
    return error_result<RuntimePageAllocation>(ErrorCode::InvalidArgument,
                                               "page count must be positive");
  }
  if (alignment == 0) {
    alignment = kDefaultPageSize;
  }
  if (!is_power_of_two(alignment)) {
    return error_result<RuntimePageAllocation>(
        ErrorCode::InvalidArgument, "page allocation alignment must be a power of two");
  }
  if ((flags & ~kHostedKnownMemoryFlags) != 0) {
    return error_result<RuntimePageAllocation>(ErrorCode::InvalidArgument,
                                               "unknown page allocation flags");
  }
  if ((flags & kRuntimeMemoryDma) != 0) {
    return error_result<RuntimePageAllocation>(
        ErrorCode::NotSupported, "hosted runtime does not support DMA memory");
  }
  if (alignment < alignof(std::max_align_t)) {
    alignment = alignof(std::max_align_t);
  }
  if (page_count > std::numeric_limits<std::size_t>::max() / kDefaultPageSize) {
    return error_result<RuntimePageAllocation>(ErrorCode::InvalidArgument,
                                               "page allocation is too large");
  }

  const auto byte_count = page_count * kDefaultPageSize;
  auto *data = ::operator new(byte_count, std::align_val_t(alignment), std::nothrow);
  if (data == nullptr) {
    return error_result<RuntimePageAllocation>(ErrorCode::Internal,
                                               "host page allocation failed");
  }
  if ((flags & kRuntimeMemoryZeroed) != 0) {
    std::memset(data, 0, byte_count);
  }

  RuntimePageAllocation allocation;
  allocation.data = data;
  allocation.page_count = page_count;
  allocation.page_size = kDefaultPageSize;
  allocation.alignment = alignment;
  allocation.flags = flags;
  allocation.physical_address = 0;
  return ok_result(allocation);
}

Status HostedPlatformRuntime::FreePages(const RuntimePageAllocation &allocation) {
  ScopedTrace trace("mattsql::HostedPlatformRuntime::FreePages",
                    "function.runtime");
  if (allocation.data == nullptr) {
    return error_status(ErrorCode::InvalidArgument, "allocation data is null");
  }
  if (!is_power_of_two(allocation.alignment)) {
    return error_status(ErrorCode::InvalidArgument,
                        "page allocation alignment must be a power of two");
  }

  ::operator delete(allocation.data, std::align_val_t(allocation.alignment));
  return ok_status();
}

Result<IoSubmissionResult>
HostedPlatformRuntime::SubmitIoBatch(std::span<const IoRequest> requests) {
  ScopedTrace trace("mattsql::HostedPlatformRuntime::SubmitIoBatch",
                    "function.runtime");
  if (requests.empty()) {
    return error_result<IoSubmissionResult>(ErrorCode::InvalidArgument,
                                            "I/O batch must be non-empty");
  }
  if (requests.size() > kHostedMaxIoBatchSize) {
    return error_result<IoSubmissionResult>(
        ErrorCode::InvalidArgument, "I/O batch exceeds hosted maximum batch size");
  }
  if (requests.size() > kHostedMaxOutstandingIo - impl_->completions.size()) {
    return error_result<IoSubmissionResult>(
        ErrorCode::IoError, "hosted I/O completion queue is full");
  }

  for (const auto &request : requests) {
    if ((request.flags & ~kHostedKnownIoRequestFlags) != 0) {
      return error_result<IoSubmissionResult>(ErrorCode::InvalidArgument,
                                              "unknown I/O request flags");
    }
    switch (request.operation) {
    case IoOperation::Read:
    case IoOperation::Write:
    case IoOperation::Flush:
      break;
    default:
      return error_result<IoSubmissionResult>(ErrorCode::InvalidArgument,
                                              "unknown I/O operation");
    }
  }

  IoSubmissionResult submission;
  submission.submitted_count = requests.size();

  for (std::size_t index = 0; index < requests.size(); ++index) {
    const auto &request = requests[index];

    auto request_id = request.id;
    if (request_id == 0) {
      request_id = impl_->next_io_request_id++;
    }
    if (index == 0) {
      submission.first_request_id = request_id;
    }

    IoCompletion completion;
    completion.id = request_id;
    completion.user_data = request.user_data;

    if (impl_->block_device == nullptr) {
      completion.status =
          error_status(ErrorCode::NotSupported, "hosted runtime has no block device");
      impl_->completions.push_back(std::move(completion));
      continue;
    }

    const auto length = effective_request_length(request);
    switch (request.operation) {
    case IoOperation::Read: {
      auto status = validate_buffered_io_request(request, length);
      if (status_ok(status)) {
        status = impl_->block_device->Read(
            request.offset,
            BufferView{std::span<std::byte>(request.buffer.bytes.data(), length)});
      }
      completion.status = std::move(status);
      completion.bytes_transferred =
          status_ok(completion.status) ? length : std::size_t{0};
      break;
    }
    case IoOperation::Write: {
      auto status = validate_buffered_io_request(request, length);
      if (status_ok(status)) {
        status = impl_->block_device->Write(
            request.offset,
            ConstBufferView{
                std::span<const std::byte>(request.buffer.bytes.data(), length)});
      }
      completion.status = std::move(status);
      completion.bytes_transferred =
          status_ok(completion.status) ? length : std::size_t{0};
      break;
    }
    case IoOperation::Flush: {
      if (length > kHostedMaxIoRequestSize) {
        completion.status = error_status(
            ErrorCode::InvalidArgument, "flush request exceeds hosted maximum size");
      } else {
        completion.status = impl_->block_device->Flush(request.offset, length);
      }
      completion.bytes_transferred = 0;
      break;
    }
    default:
      return error_result<IoSubmissionResult>(ErrorCode::InvalidArgument,
                                              "unknown I/O operation");
    }

    impl_->completions.push_back(std::move(completion));
  }

  return ok_result(submission);
}

Result<std::size_t>
HostedPlatformRuntime::PollIoCompletions(std::span<IoCompletion> completions) {
  ScopedTrace trace("mattsql::HostedPlatformRuntime::PollIoCompletions",
                    "function.runtime");
  if (completions.empty()) {
    return error_result<std::size_t>(ErrorCode::InvalidArgument,
                                     "completion output span must be non-empty");
  }

  std::size_t count = 0;
  while (count < completions.size() && !impl_->completions.empty()) {
    completions[count] = std::move(impl_->completions.front());
    impl_->completions.pop_front();
    ++count;
  }
  return ok_result(count);
}

Result<RuntimeTaskId>
HostedPlatformRuntime::SpawnTask(const TaskDescriptor &descriptor) {
  ScopedTrace trace("mattsql::HostedPlatformRuntime::SpawnTask",
                    "function.runtime");
  (void)descriptor;
  return ok_result(impl_->next_task_id++);
}

Status HostedPlatformRuntime::Yield() {
  ScopedTrace trace("mattsql::HostedPlatformRuntime::Yield", "function.runtime");
  std::this_thread::yield();
  return ok_status();
}

std::uint64_t HostedPlatformRuntime::MonotonicNanos() const {
  ScopedTrace trace("mattsql::HostedPlatformRuntime::MonotonicNanos",
                    "function.runtime");
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
