#include "mattsql/runtime/c_abi_platform_runtime.hpp"

#include "mattsql/common/result_utils.hpp"

#include <cstdlib>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace mattsql {
namespace {

inline constexpr RuntimeMemoryFlags kKnownRuntimeMemoryFlags =
    kRuntimeMemoryZeroed | kRuntimeMemoryDma;

inline constexpr IoRequestFlags kKnownIoRequestFlags =
    kIoRequestBarrierBefore | kIoRequestBarrierAfter | kIoRequestForceUnitAccess;

[[nodiscard]] ErrorCode from_abi_status_code(mattsql_abi_status_code code) {
  switch (code) {
  case MATTSQL_ABI_STATUS_OK:
    return ErrorCode::Ok;
  case MATTSQL_ABI_STATUS_INVALID_ARGUMENT:
    return ErrorCode::InvalidArgument;
  case MATTSQL_ABI_STATUS_NOT_FOUND:
    return ErrorCode::NotFound;
  case MATTSQL_ABI_STATUS_ALREADY_EXISTS:
    return ErrorCode::AlreadyExists;
  case MATTSQL_ABI_STATUS_TYPE_MISMATCH:
    return ErrorCode::TypeMismatch;
  case MATTSQL_ABI_STATUS_PARSE_ERROR:
    return ErrorCode::ParseError;
  case MATTSQL_ABI_STATUS_BIND_ERROR:
    return ErrorCode::BindError;
  case MATTSQL_ABI_STATUS_PLAN_ERROR:
    return ErrorCode::PlanError;
  case MATTSQL_ABI_STATUS_EXECUTION_ERROR:
    return ErrorCode::ExecutionError;
  case MATTSQL_ABI_STATUS_IO_ERROR:
    return ErrorCode::IoError;
  case MATTSQL_ABI_STATUS_CORRUPTION:
    return ErrorCode::Corruption;
  case MATTSQL_ABI_STATUS_TRANSACTION_CONFLICT:
    return ErrorCode::TransactionConflict;
  case MATTSQL_ABI_STATUS_NOT_SUPPORTED:
    return ErrorCode::NotSupported;
  case MATTSQL_ABI_STATUS_INTERNAL:
    return ErrorCode::Internal;
  default:
    return ErrorCode::Internal;
  }
}

[[nodiscard]] std::string from_abi_message(const mattsql_abi_status &status) {
  if (status.message == nullptr || status.message_length == 0) {
    return {};
  }
  if (status.message_length >
      static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return "ABI status message is too large";
  }
  return std::string(status.message,
                     static_cast<std::size_t>(status.message_length));
}

[[nodiscard]] Status from_abi_status(const mattsql_abi_status &status) {
  return Status{from_abi_status_code(status.code), from_abi_message(status)};
}

[[nodiscard]] bool abi_status_ok(const mattsql_abi_status &status) {
  return status.code == MATTSQL_ABI_STATUS_OK;
}

[[nodiscard]] mattsql_abi_runtime_memory_flags
to_abi_memory_flags(RuntimeMemoryFlags flags) {
  mattsql_abi_runtime_memory_flags abi_flags = MATTSQL_ABI_MEMORY_NORMAL;
  if ((flags & kRuntimeMemoryZeroed) != 0) {
    abi_flags |= MATTSQL_ABI_MEMORY_ZEROED;
  }
  if ((flags & kRuntimeMemoryDma) != 0) {
    abi_flags |= MATTSQL_ABI_MEMORY_DMA;
  }
  return abi_flags;
}

[[nodiscard]] RuntimeMemoryFlags
from_abi_memory_flags(mattsql_abi_runtime_memory_flags flags) {
  RuntimeMemoryFlags runtime_flags = kRuntimeMemoryNormal;
  if ((flags & MATTSQL_ABI_MEMORY_ZEROED) != 0) {
    runtime_flags |= kRuntimeMemoryZeroed;
  }
  if ((flags & MATTSQL_ABI_MEMORY_DMA) != 0) {
    runtime_flags |= kRuntimeMemoryDma;
  }
  return runtime_flags;
}

[[nodiscard]] bool to_abi_io_operation(IoOperation operation,
                                       mattsql_abi_io_operation &out_operation) {
  switch (operation) {
  case IoOperation::Read:
    out_operation = MATTSQL_ABI_IO_READ;
    return true;
  case IoOperation::Write:
    out_operation = MATTSQL_ABI_IO_WRITE;
    return true;
  case IoOperation::Flush:
    out_operation = MATTSQL_ABI_IO_FLUSH;
    return true;
  }
  return false;
}

[[nodiscard]] mattsql_abi_io_request_flags
to_abi_io_request_flags(IoRequestFlags flags) {
  mattsql_abi_io_request_flags abi_flags = MATTSQL_ABI_IO_REQUEST_NO_FLAGS;
  if ((flags & kIoRequestBarrierBefore) != 0) {
    abi_flags |= MATTSQL_ABI_IO_REQUEST_BARRIER_BEFORE;
  }
  if ((flags & kIoRequestBarrierAfter) != 0) {
    abi_flags |= MATTSQL_ABI_IO_REQUEST_BARRIER_AFTER;
  }
  if ((flags & kIoRequestForceUnitAccess) != 0) {
    abi_flags |= MATTSQL_ABI_IO_REQUEST_FORCE_UNIT_ACCESS;
  }
  return abi_flags;
}

[[nodiscard]] mattsql_abi_log_level to_abi_log_level(LogLevel level) {
  switch (level) {
  case LogLevel::Trace:
    return MATTSQL_ABI_LOG_TRACE;
  case LogLevel::Debug:
    return MATTSQL_ABI_LOG_DEBUG;
  case LogLevel::Info:
    return MATTSQL_ABI_LOG_INFO;
  case LogLevel::Warn:
    return MATTSQL_ABI_LOG_WARN;
  case LogLevel::Error:
    return MATTSQL_ABI_LOG_ERROR;
  case LogLevel::Fatal:
    return MATTSQL_ABI_LOG_FATAL;
  }
  return MATTSQL_ABI_LOG_ERROR;
}

[[nodiscard]] bool fits_size_t(std::uint64_t value) {
  return value <=
         static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
}

[[nodiscard]] std::size_t size_or_zero(std::uint64_t value) {
  if (!fits_size_t(value)) {
    return 0;
  }
  return static_cast<std::size_t>(value);
}

[[nodiscard]] mattsql_abi_page_allocation
to_abi_page_allocation(const RuntimePageAllocation &allocation) {
  mattsql_abi_page_allocation abi_allocation{};
  abi_allocation.data = allocation.data;
  abi_allocation.page_count = static_cast<std::uint64_t>(allocation.page_count);
  abi_allocation.page_size = static_cast<std::uint64_t>(allocation.page_size);
  abi_allocation.alignment = static_cast<std::uint64_t>(allocation.alignment);
  abi_allocation.flags = to_abi_memory_flags(allocation.flags);
  abi_allocation.physical_address = allocation.physical_address;
  return abi_allocation;
}

[[nodiscard]] RuntimePageAllocation
from_abi_page_allocation(const mattsql_abi_page_allocation &allocation) {
  RuntimePageAllocation runtime_allocation;
  runtime_allocation.data = allocation.data;
  runtime_allocation.page_count = size_or_zero(allocation.page_count);
  runtime_allocation.page_size = size_or_zero(allocation.page_size);
  runtime_allocation.alignment = size_or_zero(allocation.alignment);
  runtime_allocation.flags = from_abi_memory_flags(allocation.flags);
  runtime_allocation.physical_address = allocation.physical_address;
  return runtime_allocation;
}

[[nodiscard]] bool valid_runtime_version(const mattsql_abi_runtime_v1 &runtime) {
  return runtime.version == MATTSQL_ABI_RUNTIME_VERSION;
}

[[nodiscard]] bool is_power_of_two(std::size_t value) {
  return value != 0 && (value & (value - 1U)) == 0;
}

[[nodiscard]] bool valid_abi_bool(std::uint8_t value) { return value <= 1; }

[[nodiscard]] bool
valid_capability_booleans(const mattsql_abi_runtime_capabilities &capabilities) {
  return valid_abi_bool(capabilities.supports_async_io) &&
         valid_abi_bool(capabilities.supports_flush) &&
         valid_abi_bool(capabilities.supports_barriers) &&
         valid_abi_bool(capabilities.supports_physical_addresses) &&
         valid_abi_bool(capabilities.supports_dma_memory);
}

} // namespace

CAbiPlatformRuntime::CAbiPlatformRuntime(mattsql_abi_runtime_v1 runtime)
    : runtime_(runtime) {}

CAbiPlatformRuntime::~CAbiPlatformRuntime() = default;

CAbiPlatformRuntime::CAbiPlatformRuntime(CAbiPlatformRuntime &&) noexcept = default;

CAbiPlatformRuntime &
CAbiPlatformRuntime::operator=(CAbiPlatformRuntime &&) noexcept = default;

Status CAbiPlatformRuntime::Validate() const {
  if (!valid_runtime_version(runtime_)) {
    return error_status(ErrorCode::InvalidArgument,
                        "unsupported C ABI runtime version");
  }
  if (runtime_.get_capabilities == nullptr ||
      runtime_.allocate_pages == nullptr || runtime_.free_pages == nullptr ||
      runtime_.submit_io_batch == nullptr ||
      runtime_.poll_io_completions == nullptr ||
      runtime_.monotonic_nanos == nullptr || runtime_.log == nullptr ||
      runtime_.panic == nullptr) {
    return error_status(ErrorCode::InvalidArgument,
                        "C ABI runtime table is missing required callbacks");
  }
  return ok_status();
}

RuntimeCapabilities CAbiPlatformRuntime::GetCapabilities() const {
  RuntimeCapabilities capabilities;
  if (!status_ok(Validate())) {
    return capabilities;
  }

  mattsql_abi_runtime_capabilities abi_capabilities{};
  const auto status =
      runtime_.get_capabilities(runtime_.context, &abi_capabilities);
  if (!abi_status_ok(status)) {
    return capabilities;
  }
  if (!valid_capability_booleans(abi_capabilities)) {
    return RuntimeCapabilities{};
  }

  capabilities.page_size = size_or_zero(abi_capabilities.page_size);
  capabilities.block_size = size_or_zero(abi_capabilities.block_size);
  capabilities.max_io_request_size =
      size_or_zero(abi_capabilities.max_io_request_size);
  capabilities.max_io_batch_size =
      size_or_zero(abi_capabilities.max_io_batch_size);
  capabilities.max_outstanding_io =
      size_or_zero(abi_capabilities.max_outstanding_io);
  capabilities.supports_async_io = abi_capabilities.supports_async_io != 0;
  capabilities.supports_flush = abi_capabilities.supports_flush != 0;
  capabilities.supports_barriers = abi_capabilities.supports_barriers != 0;
  capabilities.supports_physical_addresses =
      abi_capabilities.supports_physical_addresses != 0;
  capabilities.supports_dma_memory = abi_capabilities.supports_dma_memory != 0;
  return capabilities;
}

Result<RuntimePageAllocation>
CAbiPlatformRuntime::AllocatePages(std::size_t page_count, std::size_t alignment,
                                   RuntimeMemoryFlags flags) {
  const auto validation_status = Validate();
  if (!status_ok(validation_status)) {
    return error_result<RuntimePageAllocation>(validation_status);
  }
  if ((flags & ~kKnownRuntimeMemoryFlags) != 0) {
    return error_result<RuntimePageAllocation>(ErrorCode::InvalidArgument,
                                               "unknown page allocation flags");
  }

  mattsql_abi_page_allocation allocation{};
  const auto status = runtime_.allocate_pages(
      runtime_.context, static_cast<std::uint64_t>(page_count),
      static_cast<std::uint64_t>(alignment), to_abi_memory_flags(flags),
      &allocation);
  if (!abi_status_ok(status)) {
    return error_result<RuntimePageAllocation>(from_abi_status(status));
  }
  if (!fits_size_t(allocation.page_count) ||
      !fits_size_t(allocation.page_size) || !fits_size_t(allocation.alignment)) {
    return error_result<RuntimePageAllocation>(
        ErrorCode::Internal, "C ABI page allocation metadata is too large");
  }
  if ((allocation.flags & ~kKnownRuntimeMemoryFlags) != 0) {
    return error_result<RuntimePageAllocation>(
        ErrorCode::Internal, "C ABI page allocation flags are invalid");
  }
  if (allocation.data == nullptr || allocation.page_count == 0 ||
      allocation.page_size == 0 || allocation.alignment == 0) {
    return error_result<RuntimePageAllocation>(
        ErrorCode::Internal, "C ABI page allocation metadata is invalid");
  }
  if (!is_power_of_two(static_cast<std::size_t>(allocation.alignment))) {
    return error_result<RuntimePageAllocation>(
        ErrorCode::Internal, "C ABI page allocation alignment is invalid");
  }
  return ok_result(from_abi_page_allocation(allocation));
}

Status CAbiPlatformRuntime::FreePages(
    const RuntimePageAllocation &allocation) {
  const auto validation_status = Validate();
  if (!status_ok(validation_status)) {
    return validation_status;
  }
  if ((allocation.flags & ~kKnownRuntimeMemoryFlags) != 0) {
    return error_status(ErrorCode::InvalidArgument,
                        "unknown page allocation flags");
  }
  if (allocation.data == nullptr || allocation.page_count == 0 ||
      allocation.page_size == 0 || allocation.alignment == 0 ||
      !is_power_of_two(allocation.alignment)) {
    return error_status(ErrorCode::InvalidArgument,
                        "page allocation metadata is invalid");
  }

  const auto abi_allocation = to_abi_page_allocation(allocation);
  return from_abi_status(
      runtime_.free_pages(runtime_.context, &abi_allocation));
}

Result<IoSubmissionResult>
CAbiPlatformRuntime::SubmitIoBatch(std::span<const IoRequest> requests) {
  const auto validation_status = Validate();
  if (!status_ok(validation_status)) {
    return error_result<IoSubmissionResult>(validation_status);
  }
  if (requests.empty()) {
    return error_result<IoSubmissionResult>(ErrorCode::InvalidArgument,
                                            "I/O batch must be non-empty");
  }

  std::vector<mattsql_abi_io_request> abi_requests;
  abi_requests.reserve(requests.size());
  for (const auto &request : requests) {
    mattsql_abi_io_operation operation = MATTSQL_ABI_IO_READ;
    if (!to_abi_io_operation(request.operation, operation)) {
      return error_result<IoSubmissionResult>(ErrorCode::InvalidArgument,
                                              "unknown I/O operation");
    }
    if ((request.flags & ~kKnownIoRequestFlags) != 0) {
      return error_result<IoSubmissionResult>(ErrorCode::InvalidArgument,
                                              "unknown I/O request flags");
    }
    if (request.operation != IoOperation::Flush &&
        request.length > request.buffer.bytes.size()) {
      return error_result<IoSubmissionResult>(
          ErrorCode::InvalidArgument, "I/O request length exceeds its buffer");
    }

    mattsql_abi_io_request abi_request{};
    abi_request.id = request.id;
    abi_request.operation = operation;
    abi_request.flags = to_abi_io_request_flags(request.flags);
    abi_request.offset = request.offset;
    abi_request.buffer = request.buffer.bytes.data();
    abi_request.buffer_length =
        static_cast<std::uint64_t>(request.buffer.bytes.size());
    abi_request.length = static_cast<std::uint64_t>(request.length);
    abi_request.user_data = request.user_data;
    abi_requests.push_back(abi_request);
  }

  mattsql_abi_io_submission_result abi_submission{};
  const auto status = runtime_.submit_io_batch(
      runtime_.context, abi_requests.data(),
      static_cast<std::uint64_t>(abi_requests.size()), &abi_submission);
  if (!abi_status_ok(status)) {
    return error_result<IoSubmissionResult>(from_abi_status(status));
  }
  if (!fits_size_t(abi_submission.submitted_count)) {
    return error_result<IoSubmissionResult>(
        ErrorCode::Internal, "C ABI submitted count is too large");
  }
  if (abi_submission.submitted_count !=
      static_cast<std::uint64_t>(requests.size())) {
    return error_result<IoSubmissionResult>(
        ErrorCode::Internal,
        "C ABI submitted count violates all-or-error submission");
  }
  if (abi_submission.first_request_id == 0 ||
      (requests.front().id != 0 &&
       abi_submission.first_request_id != requests.front().id)) {
    return error_result<IoSubmissionResult>(
        ErrorCode::Internal, "C ABI first request ID is invalid");
  }

  IoSubmissionResult submission;
  submission.submitted_count =
      static_cast<std::size_t>(abi_submission.submitted_count);
  submission.first_request_id = abi_submission.first_request_id;
  return ok_result(submission);
}

Result<std::size_t>
CAbiPlatformRuntime::PollIoCompletions(std::span<IoCompletion> completions) {
  const auto validation_status = Validate();
  if (!status_ok(validation_status)) {
    return error_result<std::size_t>(validation_status);
  }
  if (completions.empty()) {
    return error_result<std::size_t>(ErrorCode::InvalidArgument,
                                     "completion output span must be non-empty");
  }

  std::vector<mattsql_abi_io_completion> abi_completions(completions.size());
  std::uint64_t completion_count = 0;
  const auto status = runtime_.poll_io_completions(
      runtime_.context, abi_completions.data(),
      static_cast<std::uint64_t>(abi_completions.size()), &completion_count);
  if (!abi_status_ok(status)) {
    return error_result<std::size_t>(from_abi_status(status));
  }
  if (!fits_size_t(completion_count) ||
      completion_count > static_cast<std::uint64_t>(completions.size())) {
    return error_result<std::size_t>(
        ErrorCode::Internal, "C ABI completion count is invalid");
  }

  const auto converted_count = static_cast<std::size_t>(completion_count);
  for (std::size_t index = 0; index < converted_count; ++index) {
    const auto &abi_completion = abi_completions[index];
    if (abi_completion.id == 0) {
      return error_result<std::size_t>(
          ErrorCode::Internal, "C ABI completion request id is invalid");
    }
    if (!fits_size_t(abi_completion.bytes_transferred)) {
      return error_result<std::size_t>(
          ErrorCode::Internal, "C ABI completion byte count is too large");
    }

    completions[index].id = abi_completion.id;
    completions[index].status = from_abi_status(abi_completion.status);
    completions[index].bytes_transferred =
        static_cast<std::size_t>(abi_completion.bytes_transferred);
    completions[index].user_data = abi_completion.user_data;
  }

  return ok_result(converted_count);
}

Result<RuntimeTaskId>
CAbiPlatformRuntime::SpawnTask(const TaskDescriptor &descriptor) {
  (void)descriptor;
  return error_result<RuntimeTaskId>(
      ErrorCode::NotSupported,
      "C ABI runtime does not support task scheduling");
}

Status CAbiPlatformRuntime::Yield() {
  const auto validation_status = Validate();
  if (!status_ok(validation_status)) {
    return validation_status;
  }
  if (runtime_.yield == nullptr) {
    return ok_status();
  }
  return from_abi_status(runtime_.yield(runtime_.context));
}

std::uint64_t CAbiPlatformRuntime::MonotonicNanos() const {
  if (!status_ok(Validate())) {
    return 0;
  }

  std::uint64_t nanos = 0;
  const auto status = runtime_.monotonic_nanos(runtime_.context, &nanos);
  if (!abi_status_ok(status)) {
    return 0;
  }
  return nanos;
}

void CAbiPlatformRuntime::Log(LogLevel level, std::string_view message) {
  if (!status_ok(Validate())) {
    return;
  }
  runtime_.log(runtime_.context, to_abi_log_level(level), message.data(),
               static_cast<std::uint64_t>(message.size()));
}

[[noreturn]] void CAbiPlatformRuntime::Panic(std::string_view message) {
  if (valid_runtime_version(runtime_) && runtime_.panic != nullptr) {
    runtime_.panic(runtime_.context, message.data(),
                   static_cast<std::uint64_t>(message.size()));
  }
  std::abort();
}

} // namespace mattsql
