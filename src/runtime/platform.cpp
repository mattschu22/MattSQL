#include "mattsql/runtime/platform.hpp"

#include "mattsql/common/result_utils.hpp"

#include <cstddef>
#include <limits>
#include <span>
#include <utility>

namespace mattsql {

PlatformRuntime::~PlatformRuntime() = default;

Result<RuntimePageAllocation> PlatformRuntime::AllocatePages(std::size_t page_count) {
  return AllocatePages(page_count, kDefaultPageSize, kRuntimeMemoryZeroed);
}

Result<RuntimePageAllocationHandle>
PlatformRuntime::AllocatePageSpan(std::size_t page_count, std::size_t alignment,
                                  RuntimeMemoryFlags flags) {
  auto allocation = AllocatePages(page_count, alignment, flags);
  if (!allocation.ok()) {
    return error_result<RuntimePageAllocationHandle>(std::move(allocation.status));
  }
  if (!allocation.has_value()) {
    return error_result<RuntimePageAllocationHandle>(
        ErrorCode::Internal, "runtime returned success without a page allocation");
  }
  return ok_result(
      RuntimePageAllocationHandle(*this, std::move(allocation).TakeValue()));
}

Result<RuntimePageAllocationHandle>
PlatformRuntime::AllocatePageSpan(std::size_t page_count) {
  return AllocatePageSpan(page_count, kDefaultPageSize, kRuntimeMemoryZeroed);
}

Result<IoRequestId> PlatformRuntime::SubmitIo(const IoRequest &request) {
  const auto submission = SubmitIoBatch(std::span<const IoRequest>(&request, 1));
  if (!submission.ok()) {
    return error_result<IoRequestId>(submission.status);
  }
  if (!submission.has_value()) {
    return error_result<IoRequestId>(
        ErrorCode::Internal, "runtime returned success without an I/O submission");
  }
  if (submission.Value().submitted_count != 1) {
    return error_result<IoRequestId>(
        ErrorCode::Internal,
        "runtime returned an invalid single-request submission count");
  }
  if (submission.Value().first_request_id == 0 ||
      (request.id != 0 && submission.Value().first_request_id != request.id)) {
    return error_result<IoRequestId>(
        ErrorCode::Internal,
        "runtime returned an invalid single-request submission id");
  }
  return ok_result(submission.Value().first_request_id);
}

Result<IoCompletion> PlatformRuntime::PollIoCompletion() {
  IoCompletion completion;
  const auto count = PollIoCompletions(std::span<IoCompletion>(&completion, 1));
  if (!count.ok()) {
    return error_result<IoCompletion>(count.status);
  }
  if (!count.has_value()) {
    return error_result<IoCompletion>(
        ErrorCode::Internal, "runtime returned success without a completion count");
  }
  if (count.Value() == 0) {
    return error_result<IoCompletion>(ErrorCode::NotFound,
                                      "no I/O completions are ready");
  }
  if (count.Value() != 1) {
    return error_result<IoCompletion>(
        ErrorCode::Internal,
        "runtime returned too many single-request completions");
  }
  return ok_result(std::move(completion));
}

RuntimePageAllocationHandle::RuntimePageAllocationHandle(
    PlatformRuntime &runtime, RuntimePageAllocation allocation)
    : runtime_(&runtime), allocation_(allocation) {}

RuntimePageAllocationHandle::~RuntimePageAllocationHandle() { (void)Reset(); }

RuntimePageAllocationHandle::RuntimePageAllocationHandle(
    RuntimePageAllocationHandle &&other) noexcept
    : runtime_(std::exchange(other.runtime_, nullptr)),
      allocation_(std::exchange(other.allocation_, RuntimePageAllocation{})) {}

RuntimePageAllocationHandle &
RuntimePageAllocationHandle::operator=(RuntimePageAllocationHandle &&other) noexcept {
  if (this == &other) {
    return *this;
  }

  (void)Reset();
  runtime_ = std::exchange(other.runtime_, nullptr);
  allocation_ = std::exchange(other.allocation_, RuntimePageAllocation{});
  return *this;
}

std::span<std::byte> RuntimePageAllocationHandle::bytes() const {
  if (allocation_.data == nullptr) {
    return {};
  }
  if (allocation_.page_size != 0 &&
      allocation_.page_count >
          std::numeric_limits<std::size_t>::max() / allocation_.page_size) {
    return {};
  }
  return {static_cast<std::byte *>(allocation_.data),
          allocation_.page_count * allocation_.page_size};
}

RuntimePageAllocation RuntimePageAllocationHandle::Release() {
  runtime_ = nullptr;
  return std::exchange(allocation_, RuntimePageAllocation{});
}

Status RuntimePageAllocationHandle::Reset() {
  if (runtime_ == nullptr && allocation_.data == nullptr) {
    return ok_status();
  }
  if (runtime_ == nullptr) {
    allocation_ = RuntimePageAllocation{};
    return error_status(ErrorCode::Internal,
                        "runtime page allocation has no owning runtime");
  }

  const auto status = runtime_->FreePages(allocation_);
  runtime_ = nullptr;
  allocation_ = RuntimePageAllocation{};
  return status;
}

} // namespace mattsql
