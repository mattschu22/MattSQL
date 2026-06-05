#include "mattsql/runtime/c_abi_runtime_provider.hpp"

#include "mattsql/common/result_utils.hpp"

#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace mattsql {
namespace {

inline constexpr mattsql_abi_runtime_memory_flags kKnownAbiMemoryFlags =
    MATTSQL_ABI_MEMORY_ZEROED | MATTSQL_ABI_MEMORY_DMA;

inline constexpr mattsql_abi_io_request_flags kKnownAbiIoRequestFlags =
    MATTSQL_ABI_IO_REQUEST_BARRIER_BEFORE |
    MATTSQL_ABI_IO_REQUEST_BARRIER_AFTER |
    MATTSQL_ABI_IO_REQUEST_FORCE_UNIT_ACCESS;

inline constexpr RuntimeMemoryFlags kKnownRuntimeMemoryFlags =
    kRuntimeMemoryZeroed | kRuntimeMemoryDma;

[[nodiscard]] bool fits_size_t(std::uint64_t value) {
  return value <=
         static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
}

[[nodiscard]] std::size_t to_size_t(std::uint64_t value) {
  return static_cast<std::size_t>(value);
}

[[nodiscard]] mattsql_abi_status literal_status(mattsql_abi_status_code code,
                                                const char *message) {
  return mattsql_abi_status{code, 0, message,
                            static_cast<std::uint64_t>(std::strlen(message))};
}

[[nodiscard]] mattsql_abi_status ok_abi_status() {
  return mattsql_abi_status{MATTSQL_ABI_STATUS_OK, 0, nullptr, 0};
}

[[nodiscard]] mattsql_abi_status_code to_abi_status_code(ErrorCode code) {
  switch (code) {
  case ErrorCode::Ok:
    return MATTSQL_ABI_STATUS_OK;
  case ErrorCode::InvalidArgument:
    return MATTSQL_ABI_STATUS_INVALID_ARGUMENT;
  case ErrorCode::NotFound:
    return MATTSQL_ABI_STATUS_NOT_FOUND;
  case ErrorCode::AlreadyExists:
    return MATTSQL_ABI_STATUS_ALREADY_EXISTS;
  case ErrorCode::TypeMismatch:
    return MATTSQL_ABI_STATUS_TYPE_MISMATCH;
  case ErrorCode::ParseError:
    return MATTSQL_ABI_STATUS_PARSE_ERROR;
  case ErrorCode::BindError:
    return MATTSQL_ABI_STATUS_BIND_ERROR;
  case ErrorCode::PlanError:
    return MATTSQL_ABI_STATUS_PLAN_ERROR;
  case ErrorCode::ExecutionError:
    return MATTSQL_ABI_STATUS_EXECUTION_ERROR;
  case ErrorCode::IoError:
    return MATTSQL_ABI_STATUS_IO_ERROR;
  case ErrorCode::Corruption:
    return MATTSQL_ABI_STATUS_CORRUPTION;
  case ErrorCode::TransactionConflict:
    return MATTSQL_ABI_STATUS_TRANSACTION_CONFLICT;
  case ErrorCode::NotSupported:
    return MATTSQL_ABI_STATUS_NOT_SUPPORTED;
  case ErrorCode::Internal:
    return MATTSQL_ABI_STATUS_INTERNAL;
  }
  return MATTSQL_ABI_STATUS_INTERNAL;
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

[[nodiscard]] bool from_abi_io_operation(mattsql_abi_io_operation operation,
                                         IoOperation &out_operation) {
  switch (operation) {
  case MATTSQL_ABI_IO_READ:
    out_operation = IoOperation::Read;
    return true;
  case MATTSQL_ABI_IO_WRITE:
    out_operation = IoOperation::Write;
    return true;
  case MATTSQL_ABI_IO_FLUSH:
    out_operation = IoOperation::Flush;
    return true;
  default:
    return false;
  }
}

[[nodiscard]] IoRequestFlags
from_abi_io_request_flags(mattsql_abi_io_request_flags flags) {
  IoRequestFlags runtime_flags = kIoRequestNoFlags;
  if ((flags & MATTSQL_ABI_IO_REQUEST_BARRIER_BEFORE) != 0) {
    runtime_flags |= kIoRequestBarrierBefore;
  }
  if ((flags & MATTSQL_ABI_IO_REQUEST_BARRIER_AFTER) != 0) {
    runtime_flags |= kIoRequestBarrierAfter;
  }
  if ((flags & MATTSQL_ABI_IO_REQUEST_FORCE_UNIT_ACCESS) != 0) {
    runtime_flags |= kIoRequestForceUnitAccess;
  }
  return runtime_flags;
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
  runtime_allocation.page_count = to_size_t(allocation.page_count);
  runtime_allocation.page_size = to_size_t(allocation.page_size);
  runtime_allocation.alignment = to_size_t(allocation.alignment);
  runtime_allocation.flags = from_abi_memory_flags(allocation.flags);
  runtime_allocation.physical_address = allocation.physical_address;
  return runtime_allocation;
}

[[nodiscard]] LogLevel from_abi_log_level(mattsql_abi_log_level level) {
  switch (level) {
  case MATTSQL_ABI_LOG_TRACE:
    return LogLevel::Trace;
  case MATTSQL_ABI_LOG_DEBUG:
    return LogLevel::Debug;
  case MATTSQL_ABI_LOG_INFO:
    return LogLevel::Info;
  case MATTSQL_ABI_LOG_WARN:
    return LogLevel::Warn;
  case MATTSQL_ABI_LOG_ERROR:
    return LogLevel::Error;
  case MATTSQL_ABI_LOG_FATAL:
    return LogLevel::Fatal;
  default:
    return LogLevel::Error;
  }
}

[[nodiscard]] std::string_view message_view(const char *message,
                                            std::uint64_t message_length) {
  if (message == nullptr || message_length == 0 || !fits_size_t(message_length)) {
    return {};
  }
  return std::string_view(message, to_size_t(message_length));
}

[[nodiscard]] CAbiRuntimeProvider *provider_from_context(void *context) {
  return static_cast<CAbiRuntimeProvider *>(context);
}

[[nodiscard]] bool is_power_of_two(std::size_t value) {
  return value != 0 && (value & (value - 1U)) == 0;
}

[[nodiscard]] Status validate_runtime_allocation(
    const RuntimePageAllocation &allocation) {
  if ((allocation.flags & ~kKnownRuntimeMemoryFlags) != 0) {
    return error_status(ErrorCode::Internal,
                        "runtime returned unknown page allocation flags");
  }
  if (allocation.data == nullptr || allocation.page_count == 0 ||
      allocation.page_size == 0 || allocation.alignment == 0 ||
      !is_power_of_two(allocation.alignment)) {
    return error_status(ErrorCode::Internal,
                        "runtime returned invalid page allocation metadata");
  }
  return ok_status();
}

} // namespace

CAbiRuntimeProvider::CAbiRuntimeProvider(PlatformRuntime &runtime)
    : runtime_(&runtime) {
  runtime_table_.version = MATTSQL_ABI_RUNTIME_VERSION;
  runtime_table_.context = this;
  runtime_table_.get_capabilities = &CAbiRuntimeProvider::GetCapabilities;
  runtime_table_.allocate_pages = &CAbiRuntimeProvider::AllocatePages;
  runtime_table_.free_pages = &CAbiRuntimeProvider::FreePages;
  runtime_table_.submit_io_batch = &CAbiRuntimeProvider::SubmitIoBatch;
  runtime_table_.poll_io_completions = &CAbiRuntimeProvider::PollIoCompletions;
  runtime_table_.yield = &CAbiRuntimeProvider::Yield;
  runtime_table_.monotonic_nanos = &CAbiRuntimeProvider::MonotonicNanos;
  runtime_table_.log = &CAbiRuntimeProvider::Log;
  runtime_table_.panic = &CAbiRuntimeProvider::Panic;
}

mattsql_abi_runtime_v1 CAbiRuntimeProvider::RuntimeTable() const {
  return runtime_table_;
}

PlatformRuntime &CAbiRuntimeProvider::GetRuntime() const { return *runtime_; }

mattsql_abi_status CAbiRuntimeProvider::StoreStatus(
    const Status &status) const {
  if (status_ok(status)) {
    status_message_.clear();
    return ok_abi_status();
  }

  status_message_ = status.message;
  if (status_message_.empty()) {
    return mattsql_abi_status{to_abi_status_code(status.code), 0, nullptr, 0};
  }
  return mattsql_abi_status{to_abi_status_code(status.code), 0,
                            status_message_.c_str(),
                            static_cast<std::uint64_t>(status_message_.size())};
}

mattsql_abi_status CAbiRuntimeProvider::StoreError(
    ErrorCode code, std::string_view message) const {
  return StoreStatus(Status{code, std::string(message.data(), message.size())});
}

mattsql_abi_status CAbiRuntimeProvider::GetCapabilities(
    void *context, mattsql_abi_runtime_capabilities *out_capabilities) {
  auto *provider = provider_from_context(context);
  if (provider == nullptr || out_capabilities == nullptr) {
    return literal_status(MATTSQL_ABI_STATUS_INVALID_ARGUMENT,
                          "missing capabilities argument");
  }

  try {
    const auto capabilities = provider->runtime_->GetCapabilities();
    *out_capabilities = {};
    out_capabilities->page_size = static_cast<std::uint64_t>(capabilities.page_size);
    out_capabilities->block_size = static_cast<std::uint64_t>(capabilities.block_size);
    out_capabilities->max_io_request_size =
        static_cast<std::uint64_t>(capabilities.max_io_request_size);
    out_capabilities->max_io_batch_size =
        static_cast<std::uint64_t>(capabilities.max_io_batch_size);
    out_capabilities->max_outstanding_io =
        static_cast<std::uint64_t>(capabilities.max_outstanding_io);
    out_capabilities->supports_async_io =
        capabilities.supports_async_io ? std::uint8_t{1} : std::uint8_t{0};
    out_capabilities->supports_flush =
        capabilities.supports_flush ? std::uint8_t{1} : std::uint8_t{0};
    out_capabilities->supports_barriers =
        capabilities.supports_barriers ? std::uint8_t{1} : std::uint8_t{0};
    out_capabilities->supports_physical_addresses =
        capabilities.supports_physical_addresses ? std::uint8_t{1}
                                                 : std::uint8_t{0};
    out_capabilities->supports_dma_memory =
        capabilities.supports_dma_memory ? std::uint8_t{1} : std::uint8_t{0};
    return ok_abi_status();
  } catch (...) {
    return provider->StoreError(ErrorCode::Internal,
                                "exception escaped capability callback");
  }
}

mattsql_abi_status CAbiRuntimeProvider::AllocatePages(
    void *context, std::uint64_t page_count, std::uint64_t alignment,
    mattsql_abi_runtime_memory_flags flags,
    mattsql_abi_page_allocation *out_allocation) {
  auto *provider = provider_from_context(context);
  if (provider == nullptr || out_allocation == nullptr) {
    return literal_status(MATTSQL_ABI_STATUS_INVALID_ARGUMENT,
                          "invalid page allocation request");
  }
  if ((flags & ~kKnownAbiMemoryFlags) != 0) {
    return provider->StoreError(ErrorCode::InvalidArgument,
                                "unknown page allocation flags");
  }
  if (!fits_size_t(page_count) || !fits_size_t(alignment)) {
    return provider->StoreError(ErrorCode::InvalidArgument,
                                "page allocation metadata is too large");
  }

  try {
    auto allocation = provider->runtime_->AllocatePages(
        to_size_t(page_count), to_size_t(alignment), from_abi_memory_flags(flags));
    if (!status_ok(allocation.status)) {
      return provider->StoreStatus(allocation.status);
    }
    if (!allocation.value.has_value()) {
      return provider->StoreError(
          ErrorCode::Internal,
          "runtime returned success without a page allocation");
    }
    const auto allocation_status = validate_runtime_allocation(*allocation.value);
    if (!status_ok(allocation_status)) {
      return provider->StoreStatus(allocation_status);
    }

    *out_allocation = to_abi_page_allocation(*allocation.value);
    return ok_abi_status();
  } catch (...) {
    return provider->StoreError(ErrorCode::Internal,
                                "exception escaped page allocation callback");
  }
}

mattsql_abi_status CAbiRuntimeProvider::FreePages(
    void *context, const mattsql_abi_page_allocation *allocation) {
  auto *provider = provider_from_context(context);
  if (provider == nullptr || allocation == nullptr) {
    return literal_status(MATTSQL_ABI_STATUS_INVALID_ARGUMENT,
                          "invalid page allocation");
  }
  if ((allocation->flags & ~kKnownAbiMemoryFlags) != 0) {
    return provider->StoreError(ErrorCode::InvalidArgument,
                                "unknown page allocation flags");
  }
  if (!fits_size_t(allocation->page_count) || !fits_size_t(allocation->page_size) ||
      !fits_size_t(allocation->alignment)) {
    return provider->StoreError(ErrorCode::InvalidArgument,
                                "page allocation metadata is too large");
  }
  if (allocation->data == nullptr || allocation->page_count == 0 ||
      allocation->page_size == 0 || allocation->alignment == 0 ||
      !is_power_of_two(to_size_t(allocation->alignment))) {
    return provider->StoreError(ErrorCode::InvalidArgument,
                                "page allocation metadata is invalid");
  }

  try {
    return provider->StoreStatus(
        provider->runtime_->FreePages(from_abi_page_allocation(*allocation)));
  } catch (...) {
    return provider->StoreError(ErrorCode::Internal,
                                "exception escaped page free callback");
  }
}

mattsql_abi_status CAbiRuntimeProvider::SubmitIoBatch(
    void *context, const mattsql_abi_io_request *requests,
    std::uint64_t request_count,
    mattsql_abi_io_submission_result *out_submission) {
  auto *provider = provider_from_context(context);
  if (provider == nullptr || requests == nullptr || request_count == 0 ||
      out_submission == nullptr) {
    return literal_status(MATTSQL_ABI_STATUS_INVALID_ARGUMENT,
                          "invalid I/O batch");
  }
  if (!fits_size_t(request_count)) {
    return provider->StoreError(ErrorCode::InvalidArgument,
                                "I/O batch is too large");
  }

  try {
    std::vector<IoRequest> runtime_requests;
    runtime_requests.reserve(to_size_t(request_count));

    for (std::uint64_t index = 0; index < request_count; ++index) {
      const auto &abi_request = requests[index];
      IoOperation operation = IoOperation::Read;
      if (!from_abi_io_operation(abi_request.operation, operation)) {
        return provider->StoreError(ErrorCode::InvalidArgument,
                                    "unknown I/O operation");
      }
      if ((abi_request.flags & ~kKnownAbiIoRequestFlags) != 0) {
        return provider->StoreError(ErrorCode::InvalidArgument,
                                    "unknown I/O request flags");
      }
      if (!fits_size_t(abi_request.buffer_length) ||
          !fits_size_t(abi_request.length)) {
        return provider->StoreError(ErrorCode::InvalidArgument,
                                    "I/O request length is too large");
      }
      if (operation != IoOperation::Flush && abi_request.buffer == nullptr) {
        return provider->StoreError(ErrorCode::InvalidArgument,
                                    "buffered I/O request has null buffer");
      }
      if (abi_request.buffer == nullptr && abi_request.buffer_length != 0) {
        return provider->StoreError(ErrorCode::InvalidArgument,
                                    "I/O request has null buffer with length");
      }
      if (operation != IoOperation::Flush &&
          abi_request.length > abi_request.buffer_length) {
        return provider->StoreError(ErrorCode::InvalidArgument,
                                    "I/O request length exceeds its buffer");
      }

      IoRequest request;
      request.id = abi_request.id;
      request.operation = operation;
      request.offset = abi_request.offset;
      request.buffer = BufferView{std::span<std::byte>(
          static_cast<std::byte *>(abi_request.buffer),
          to_size_t(abi_request.buffer_length))};
      request.length = to_size_t(abi_request.length);
      request.flags = from_abi_io_request_flags(abi_request.flags);
      request.user_data = abi_request.user_data;
      runtime_requests.push_back(request);
    }

    auto submission = provider->runtime_->SubmitIoBatch(std::span<const IoRequest>(
        runtime_requests.data(), runtime_requests.size()));
    if (!status_ok(submission.status)) {
      return provider->StoreStatus(submission.status);
    }
    if (!submission.value.has_value()) {
      return provider->StoreError(
          ErrorCode::Internal,
          "runtime returned success without an I/O submission");
    }
    if (submission.value->submitted_count != runtime_requests.size()) {
      return provider->StoreError(
          ErrorCode::Internal,
          "runtime returned an invalid I/O submission count");
    }
    if (submission.value->first_request_id == 0 ||
        (runtime_requests.front().id != 0 &&
         submission.value->first_request_id != runtime_requests.front().id)) {
      return provider->StoreError(
          ErrorCode::Internal,
          "runtime returned an invalid first I/O request id");
    }

    *out_submission = {};
    out_submission->submitted_count =
        static_cast<std::uint64_t>(submission.value->submitted_count);
    out_submission->first_request_id = submission.value->first_request_id;
    return ok_abi_status();
  } catch (...) {
    return provider->StoreError(ErrorCode::Internal,
                                "exception escaped I/O submission callback");
  }
}

mattsql_abi_status CAbiRuntimeProvider::PollIoCompletions(
    void *context, mattsql_abi_io_completion *completions,
    std::uint64_t max_completion_count, std::uint64_t *out_completion_count) {
  auto *provider = provider_from_context(context);
  if (provider == nullptr || completions == nullptr || max_completion_count == 0 ||
      out_completion_count == nullptr) {
    return literal_status(MATTSQL_ABI_STATUS_INVALID_ARGUMENT,
                          "invalid completion poll");
  }
  if (!fits_size_t(max_completion_count)) {
    return provider->StoreError(ErrorCode::InvalidArgument,
                                "completion batch is too large");
  }

  try {
    std::vector<IoCompletion> runtime_completions(to_size_t(max_completion_count));
    auto completion_count = provider->runtime_->PollIoCompletions(
        std::span<IoCompletion>(runtime_completions.data(),
                                runtime_completions.size()));
    if (!status_ok(completion_count.status)) {
      return provider->StoreStatus(completion_count.status);
    }
    if (!completion_count.value.has_value()) {
      return provider->StoreError(
          ErrorCode::Internal,
          "runtime returned success without a completion count");
    }
    if (*completion_count.value > runtime_completions.size()) {
      return provider->StoreError(ErrorCode::Internal,
                                  "runtime returned too many completions");
    }

    provider->completion_status_messages_.clear();
    provider->completion_status_messages_.reserve(*completion_count.value);

    for (std::size_t index = 0; index < *completion_count.value; ++index) {
      const auto &completion = runtime_completions[index];
      auto &abi_completion = completions[index];
      abi_completion = {};
      abi_completion.id = completion.id;
      abi_completion.status.code = to_abi_status_code(completion.status.code);
      if (!completion.status.message.empty()) {
        provider->completion_status_messages_.push_back(completion.status.message);
        const auto &message = provider->completion_status_messages_.back();
        abi_completion.status.message = message.c_str();
        abi_completion.status.message_length =
            static_cast<std::uint64_t>(message.size());
      }
      abi_completion.bytes_transferred =
          static_cast<std::uint64_t>(completion.bytes_transferred);
      abi_completion.user_data = completion.user_data;
    }

    *out_completion_count = static_cast<std::uint64_t>(*completion_count.value);
    return ok_abi_status();
  } catch (...) {
    return provider->StoreError(ErrorCode::Internal,
                                "exception escaped completion poll callback");
  }
}

mattsql_abi_status CAbiRuntimeProvider::Yield(void *context) {
  auto *provider = provider_from_context(context);
  if (provider == nullptr) {
    return literal_status(MATTSQL_ABI_STATUS_INVALID_ARGUMENT, "missing context");
  }

  try {
    return provider->StoreStatus(provider->runtime_->Yield());
  } catch (...) {
    return provider->StoreError(ErrorCode::Internal,
                                "exception escaped yield callback");
  }
}

mattsql_abi_status CAbiRuntimeProvider::MonotonicNanos(
    void *context, std::uint64_t *out_nanos) {
  auto *provider = provider_from_context(context);
  if (provider == nullptr || out_nanos == nullptr) {
    return literal_status(MATTSQL_ABI_STATUS_INVALID_ARGUMENT,
                          "missing clock output");
  }

  try {
    *out_nanos = provider->runtime_->MonotonicNanos();
    return ok_abi_status();
  } catch (...) {
    return provider->StoreError(ErrorCode::Internal,
                                "exception escaped clock callback");
  }
}

void CAbiRuntimeProvider::Log(void *context, mattsql_abi_log_level level,
                              const char *message,
                              std::uint64_t message_length) {
  auto *provider = provider_from_context(context);
  if (provider == nullptr) {
    return;
  }

  try {
    provider->runtime_->Log(from_abi_log_level(level),
                            message_view(message, message_length));
  } catch (...) {
  }
}

[[noreturn]] void CAbiRuntimeProvider::Panic(void *context, const char *message,
                                             std::uint64_t message_length) {
  auto *provider = provider_from_context(context);
  if (provider != nullptr) {
    provider->runtime_->Panic(message_view(message, message_length));
  }
  std::abort();
}

} // namespace mattsql
