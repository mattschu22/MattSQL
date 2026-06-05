#include "mattsql/common/result_utils.hpp"
#include "mattsql/runtime/c_abi_platform_runtime.hpp"
#include "mattsql/storage/memory_block_device.hpp"

#include "mattsql_test_utils.hpp"
#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <new>
#include <span>
#include <string>
#include <vector>

namespace {

mattsql_abi_status abi_ok() {
  return mattsql_abi_status{MATTSQL_ABI_STATUS_OK, 0, nullptr, 0};
}

mattsql_abi_status abi_error(mattsql_abi_status_code code, const char *message) {
  return mattsql_abi_status{code, 0, message, std::strlen(message)};
}

struct FakeAbiRuntimeState {
  FakeAbiRuntimeState() : device(128, 32) {}

  mattsql::MemoryBlockDevice device;
  std::deque<mattsql_abi_io_completion> completions;
  std::uint64_t next_request_id = 1;
  std::uint64_t nanos = 123456789;
  bool yielded = false;
  bool fail_yield = false;
  bool fail_clock = false;
  bool return_invalid_capability_boolean = false;
  bool return_invalid_allocation_metadata = false;
  bool return_unknown_allocation_flags = false;
  bool return_non_power_allocation_alignment = false;
  bool return_invalid_submission_count = false;
  bool return_invalid_first_request_id = false;
  bool return_invalid_completion_count = false;
  bool return_zero_completion_id = false;
  mattsql_abi_log_level last_log_level = MATTSQL_ABI_LOG_TRACE;
  std::string last_log_message;
};

mattsql_abi_status
fake_get_capabilities(void *context,
                      mattsql_abi_runtime_capabilities *out_capabilities) {
  if (context == nullptr || out_capabilities == nullptr) {
    return abi_error(MATTSQL_ABI_STATUS_INVALID_ARGUMENT,
                     "missing capabilities argument");
  }

  const auto *state = static_cast<FakeAbiRuntimeState *>(context);
  *out_capabilities = {};
  out_capabilities->page_size = mattsql::kDefaultPageSize;
  out_capabilities->block_size = state->device.BlockSize();
  out_capabilities->max_io_request_size = mattsql::kDefaultPageSize;
  out_capabilities->max_io_batch_size = 8;
  out_capabilities->max_outstanding_io = 16;
  out_capabilities->supports_async_io = 0;
  out_capabilities->supports_flush = 1;
  out_capabilities->supports_barriers = 1;
  out_capabilities->supports_physical_addresses = 0;
  out_capabilities->supports_dma_memory = 0;
  if (state->return_invalid_capability_boolean) {
    out_capabilities->supports_flush = 2;
  }
  return abi_ok();
}

mattsql_abi_status fake_allocate_pages(void *context, std::uint64_t page_count,
                                       std::uint64_t alignment,
                                       mattsql_abi_runtime_memory_flags flags,
                                       mattsql_abi_page_allocation *out_allocation) {
  if (page_count == 0 || out_allocation == nullptr) {
    return abi_error(MATTSQL_ABI_STATUS_INVALID_ARGUMENT,
                     "invalid page allocation request");
  }
  if ((flags & MATTSQL_ABI_MEMORY_DMA) != 0) {
    return abi_error(MATTSQL_ABI_STATUS_NOT_SUPPORTED, "DMA memory is not supported");
  }

  auto *state = static_cast<FakeAbiRuntimeState *>(context);
  if (state != nullptr && state->return_unknown_allocation_flags) {
    *out_allocation = {};
    out_allocation->data = reinterpret_cast<void *>(std::uintptr_t{0x1000});
    out_allocation->page_count = 1;
    out_allocation->page_size = mattsql::kDefaultPageSize;
    out_allocation->alignment = alignment;
    out_allocation->flags = flags | (1U << 12U);
    return abi_ok();
  }
  if (state != nullptr && state->return_non_power_allocation_alignment) {
    *out_allocation = {};
    out_allocation->data = reinterpret_cast<void *>(std::uintptr_t{0x1000});
    out_allocation->page_count = 1;
    out_allocation->page_size = mattsql::kDefaultPageSize;
    out_allocation->alignment = 3;
    out_allocation->flags = flags;
    return abi_ok();
  }
  if (state != nullptr && state->return_invalid_allocation_metadata) {
    *out_allocation = {};
    out_allocation->page_count = 1;
    out_allocation->page_size = mattsql::kDefaultPageSize;
    out_allocation->alignment = alignment;
    out_allocation->flags = flags;
    return abi_ok();
  }

  const auto byte_count =
      page_count * static_cast<std::uint64_t>(mattsql::kDefaultPageSize);
  auto *data = ::operator new(static_cast<std::size_t>(byte_count),
                              std::align_val_t(static_cast<std::size_t>(alignment)),
                              std::nothrow);
  if (data == nullptr) {
    return abi_error(MATTSQL_ABI_STATUS_INTERNAL, "allocation failed");
  }
  if ((flags & MATTSQL_ABI_MEMORY_ZEROED) != 0) {
    std::memset(data, 0, static_cast<std::size_t>(byte_count));
  }

  *out_allocation = {};
  out_allocation->data = data;
  out_allocation->page_count = page_count;
  out_allocation->page_size = mattsql::kDefaultPageSize;
  out_allocation->alignment = alignment;
  out_allocation->flags = flags;
  out_allocation->physical_address = 0;
  return abi_ok();
}

mattsql_abi_status fake_free_pages(void *context,
                                   const mattsql_abi_page_allocation *allocation) {
  (void)context;
  if (allocation == nullptr || allocation->data == nullptr) {
    return abi_error(MATTSQL_ABI_STATUS_INVALID_ARGUMENT, "invalid page allocation");
  }
  ::operator delete(allocation->data,
                    std::align_val_t(static_cast<std::size_t>(allocation->alignment)));
  return abi_ok();
}

mattsql_abi_status
fake_submit_io_batch(void *context, const mattsql_abi_io_request *requests,
                     std::uint64_t request_count,
                     mattsql_abi_io_submission_result *out_submission) {
  if (context == nullptr || requests == nullptr || out_submission == nullptr ||
      request_count == 0) {
    return abi_error(MATTSQL_ABI_STATUS_INVALID_ARGUMENT, "invalid I/O batch");
  }

  auto *state = static_cast<FakeAbiRuntimeState *>(context);
  *out_submission = {};
  out_submission->submitted_count = request_count;
  if (state->return_invalid_submission_count) {
    out_submission->submitted_count = request_count + 1;
    out_submission->first_request_id = 1;
    return abi_ok();
  }
  if (state->return_invalid_first_request_id) {
    out_submission->first_request_id = 0;
    return abi_ok();
  }

  for (std::uint64_t index = 0; index < request_count; ++index) {
    const auto &request = requests[index];
    const auto request_id = request.id == 0 ? state->next_request_id++ : request.id;
    if (index == 0) {
      out_submission->first_request_id = request_id;
    }

    const auto length = request.length == 0 ? request.buffer_length : request.length;
    mattsql_abi_io_completion completion{};
    completion.id = request_id;
    completion.status = abi_ok();
    completion.user_data = request.user_data;

    if (request.operation == MATTSQL_ABI_IO_READ) {
      auto *data = static_cast<std::byte *>(request.buffer);
      completion.status =
          mattsql::status_ok(state->device.Read(
              request.offset, mattsql::BufferView{std::span<std::byte>(
                                  data, static_cast<std::size_t>(length))}))
              ? abi_ok()
              : abi_error(MATTSQL_ABI_STATUS_IO_ERROR, "read failed");
      completion.bytes_transferred =
          completion.status.code == MATTSQL_ABI_STATUS_OK ? length : 0;
    } else if (request.operation == MATTSQL_ABI_IO_WRITE) {
      auto *data = static_cast<std::byte *>(request.buffer);
      completion.status =
          mattsql::status_ok(state->device.Write(
              request.offset, mattsql::ConstBufferView{std::span<const std::byte>(
                                  data, static_cast<std::size_t>(length))}))
              ? abi_ok()
              : abi_error(MATTSQL_ABI_STATUS_IO_ERROR, "write failed");
      completion.bytes_transferred =
          completion.status.code == MATTSQL_ABI_STATUS_OK ? length : 0;
    } else if (request.operation == MATTSQL_ABI_IO_FLUSH) {
      completion.status = mattsql::status_ok(state->device.Flush(
                              request.offset, static_cast<std::size_t>(length)))
                              ? abi_ok()
                              : abi_error(MATTSQL_ABI_STATUS_IO_ERROR, "flush failed");
    } else {
      completion.status =
          abi_error(MATTSQL_ABI_STATUS_INVALID_ARGUMENT, "bad operation");
    }

    state->completions.push_back(completion);
  }

  return abi_ok();
}

mattsql_abi_status fake_poll_io_completions(void *context,
                                            mattsql_abi_io_completion *completions,
                                            std::uint64_t max_completion_count,
                                            std::uint64_t *out_completion_count) {
  if (context == nullptr || completions == nullptr || out_completion_count == nullptr ||
      max_completion_count == 0) {
    return abi_error(MATTSQL_ABI_STATUS_INVALID_ARGUMENT, "invalid completion poll");
  }

  auto *state = static_cast<FakeAbiRuntimeState *>(context);
  if (state->return_zero_completion_id) {
    completions[0] = {};
    completions[0].id = 0;
    completions[0].status = abi_ok();
    *out_completion_count = 1;
    return abi_ok();
  }
  if (state->return_invalid_completion_count) {
    *out_completion_count = max_completion_count + 1;
    return abi_ok();
  }

  std::uint64_t count = 0;
  while (count < max_completion_count && !state->completions.empty()) {
    completions[count] = state->completions.front();
    state->completions.pop_front();
    ++count;
  }
  *out_completion_count = count;
  return abi_ok();
}

mattsql_abi_status fake_yield(void *context) {
  if (context == nullptr) {
    return abi_error(MATTSQL_ABI_STATUS_INVALID_ARGUMENT, "missing context");
  }
  auto *state = static_cast<FakeAbiRuntimeState *>(context);
  if (state->fail_yield) {
    return abi_error(MATTSQL_ABI_STATUS_INTERNAL, "yield failed");
  }
  state->yielded = true;
  return abi_ok();
}

mattsql_abi_status fake_monotonic_nanos(void *context, std::uint64_t *out_nanos) {
  if (context == nullptr || out_nanos == nullptr) {
    return abi_error(MATTSQL_ABI_STATUS_INVALID_ARGUMENT, "missing clock output");
  }
  auto *state = static_cast<FakeAbiRuntimeState *>(context);
  if (state->fail_clock) {
    return abi_error(MATTSQL_ABI_STATUS_INTERNAL, "clock failed");
  }
  *out_nanos = state->nanos;
  return abi_ok();
}

void fake_log(void *context, mattsql_abi_log_level level, const char *message,
              std::uint64_t message_length) {
  auto *state = static_cast<FakeAbiRuntimeState *>(context);
  state->last_log_level = level;
  state->last_log_message.assign(message, static_cast<std::size_t>(message_length));
}

void fake_panic(void *context, const char *message, std::uint64_t message_length) {
  fake_log(context, MATTSQL_ABI_LOG_FATAL, message, message_length);
}

mattsql_abi_runtime_v1 make_fake_runtime(FakeAbiRuntimeState &state) {
  mattsql_abi_runtime_v1 runtime{};
  runtime.version = MATTSQL_ABI_RUNTIME_VERSION;
  runtime.context = &state;
  runtime.get_capabilities = &fake_get_capabilities;
  runtime.allocate_pages = &fake_allocate_pages;
  runtime.free_pages = &fake_free_pages;
  runtime.submit_io_batch = &fake_submit_io_batch;
  runtime.poll_io_completions = &fake_poll_io_completions;
  runtime.yield = &fake_yield;
  runtime.monotonic_nanos = &fake_monotonic_nanos;
  runtime.log = &fake_log;
  runtime.panic = &fake_panic;
  return runtime;
}

} // namespace

/// Verifies the C ABI adapter rejects unusable runtime tables before dispatch.
TEST_CASE(c_abi_runtime_validates_required_table) {
  mattsql_abi_runtime_v1 missing_callbacks{};
  missing_callbacks.version = MATTSQL_ABI_RUNTIME_VERSION;
  mattsql::CAbiPlatformRuntime missing_runtime(missing_callbacks);
  EXPECT_TRUE(missing_runtime.Validate().code == mattsql::ErrorCode::InvalidArgument);

  FakeAbiRuntimeState state;
  auto wrong_version = make_fake_runtime(state);
  wrong_version.version = MATTSQL_ABI_RUNTIME_VERSION + 1;
  mattsql::CAbiPlatformRuntime wrong_version_runtime(wrong_version);
  EXPECT_TRUE(wrong_version_runtime.Validate().code ==
              mattsql::ErrorCode::InvalidArgument);
}

/// Verifies the ABI adapter converts capabilities, page allocation, time, and logs.
TEST_CASE(c_abi_runtime_adapts_capabilities_memory_time_and_logs) {
  FakeAbiRuntimeState state;
  mattsql::CAbiPlatformRuntime runtime(make_fake_runtime(state));

  EXPECT_TRUE(mattsql::status_ok(runtime.Validate()));
  const auto capabilities = runtime.GetCapabilities();
  EXPECT_EQ(capabilities.page_size, mattsql::kDefaultPageSize);
  EXPECT_EQ(capabilities.block_size, 32U);
  EXPECT_EQ(capabilities.max_io_batch_size, 8U);
  EXPECT_TRUE(capabilities.supports_flush);
  EXPECT_TRUE(capabilities.supports_barriers);
  EXPECT_TRUE(!capabilities.supports_dma_memory);

  state.return_invalid_capability_boolean = true;
  const auto invalid_capabilities = runtime.GetCapabilities();
  EXPECT_EQ(invalid_capabilities.block_size, 0U);
  EXPECT_TRUE(!invalid_capabilities.supports_flush);
  state.return_invalid_capability_boolean = false;

  auto allocation_result =
      runtime.AllocatePages(1, 8192, mattsql::kRuntimeMemoryZeroed);
  EXPECT_TRUE(mattsql::status_ok(allocation_result.status));
  EXPECT_TRUE(allocation_result.value.has_value());
  auto allocation = *allocation_result.value;
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(allocation.data) % 8192U,
            std::uintptr_t{0});
  EXPECT_EQ(allocation.flags, mattsql::kRuntimeMemoryZeroed);
  auto *bytes = static_cast<std::byte *>(allocation.data);
  EXPECT_TRUE(bytes[0] == std::byte{0});
  EXPECT_TRUE(mattsql::status_ok(runtime.FreePages(allocation)));

  auto dma_allocation = runtime.AllocatePages(1, 8192, mattsql::kRuntimeMemoryDma);
  EXPECT_TRUE(dma_allocation.status.code == mattsql::ErrorCode::NotSupported);

  auto unknown_flag_allocation =
      runtime.AllocatePages(1, 8192, std::uint32_t{1U << 12U});
  EXPECT_TRUE(unknown_flag_allocation.status.code ==
              mattsql::ErrorCode::InvalidArgument);

  auto *bad_free_data =
      ::operator new(mattsql::kDefaultPageSize, std::align_val_t(8192));
  mattsql::RuntimePageAllocation bad_free_allocation;
  bad_free_allocation.data = bad_free_data;
  bad_free_allocation.page_count = 1;
  bad_free_allocation.page_size = mattsql::kDefaultPageSize;
  bad_free_allocation.alignment = 8192;
  bad_free_allocation.flags = std::uint32_t{1U << 12U};
  const auto bad_free_status = runtime.FreePages(bad_free_allocation);
  EXPECT_TRUE(bad_free_status.code == mattsql::ErrorCode::InvalidArgument);
  if (bad_free_status.code == mattsql::ErrorCode::InvalidArgument) {
    ::operator delete(bad_free_data, std::align_val_t(8192));
  }

  EXPECT_EQ(runtime.MonotonicNanos(), std::uint64_t{123456789});
  EXPECT_TRUE(mattsql::status_ok(runtime.Yield()));
  EXPECT_TRUE(state.yielded);

  runtime.Log(mattsql::LogLevel::Warn, "hello ABI");
  EXPECT_EQ(state.last_log_level, MATTSQL_ABI_LOG_WARN);
  EXPECT_EQ(state.last_log_message, std::string("hello ABI"));
}

/// Verifies batch-shaped C ABI I/O round-trips through the C++ runtime adapter.
TEST_CASE(c_abi_runtime_adapts_batched_io) {
  FakeAbiRuntimeState state;
  mattsql::CAbiPlatformRuntime runtime(make_fake_runtime(state));
  std::vector<std::byte> source(32, std::byte{0x7A});
  std::vector<std::byte> output(32, std::byte{0});

  std::vector<mattsql::IoRequest> batch(2);
  batch[0].operation = mattsql::IoOperation::Write;
  batch[0].offset = 0;
  batch[0].buffer = test::mutable_view(source);
  batch[0].user_data = 41;
  batch[1].id = 22;
  batch[1].operation = mattsql::IoOperation::Flush;
  batch[1].offset = 0;
  batch[1].length = 32;
  batch[1].flags = mattsql::kIoRequestBarrierAfter;
  batch[1].user_data = 42;

  auto submission = runtime.SubmitIoBatch(
      std::span<const mattsql::IoRequest>(batch.data(), batch.size()));
  EXPECT_TRUE(mattsql::status_ok(submission.status));
  EXPECT_TRUE(submission.value.has_value());
  EXPECT_EQ(submission.value->submitted_count, 2U);
  EXPECT_EQ(submission.value->first_request_id, mattsql::IoRequestId{1});

  std::vector<mattsql::IoCompletion> completions(2);
  auto count = runtime.PollIoCompletions(
      std::span<mattsql::IoCompletion>(completions.data(), completions.size()));
  EXPECT_TRUE(mattsql::status_ok(count.status));
  EXPECT_TRUE(count.value.has_value());
  EXPECT_EQ(*count.value, 2U);
  EXPECT_EQ(completions[0].id, mattsql::IoRequestId{1});
  EXPECT_TRUE(mattsql::status_ok(completions[0].status));
  EXPECT_EQ(completions[0].bytes_transferred, 32U);
  EXPECT_EQ(completions[0].user_data, std::uintptr_t{41});
  EXPECT_EQ(completions[1].id, mattsql::IoRequestId{22});
  EXPECT_TRUE(mattsql::status_ok(completions[1].status));
  EXPECT_EQ(completions[1].user_data, std::uintptr_t{42});

  mattsql::IoRequest read_request;
  read_request.operation = mattsql::IoOperation::Read;
  read_request.offset = 0;
  read_request.buffer = test::mutable_view(output);

  auto read_submission = runtime.SubmitIo(read_request);
  EXPECT_TRUE(mattsql::status_ok(read_submission.status));
  auto read_completion = runtime.PollIoCompletion();
  EXPECT_TRUE(mattsql::status_ok(read_completion.status));
  EXPECT_TRUE(read_completion.value.has_value());
  EXPECT_TRUE(mattsql::status_ok(read_completion.value->status));
  test::expect_bytes_equal(output, source);
}

/// Verifies the adapter enforces local ABI contract checks before dispatch.
TEST_CASE(c_abi_runtime_rejects_invalid_adapter_requests) {
  FakeAbiRuntimeState state;
  mattsql::CAbiPlatformRuntime runtime(make_fake_runtime(state));
  std::vector<std::byte> buffer(32);

  auto empty_submission = runtime.SubmitIoBatch({});
  EXPECT_TRUE(empty_submission.status.code == mattsql::ErrorCode::InvalidArgument);

  mattsql::IoRequest bad_length;
  bad_length.operation = mattsql::IoOperation::Write;
  bad_length.buffer = test::mutable_view(buffer);
  bad_length.length = 64;
  auto bad_submission =
      runtime.SubmitIoBatch(std::span<const mattsql::IoRequest>(&bad_length, 1));
  EXPECT_TRUE(bad_submission.status.code == mattsql::ErrorCode::InvalidArgument);

  mattsql::IoRequest bad_flags;
  bad_flags.operation = mattsql::IoOperation::Write;
  bad_flags.buffer = test::mutable_view(buffer);
  bad_flags.flags = std::uint32_t{1U << 12U};
  auto bad_flags_submission =
      runtime.SubmitIoBatch(std::span<const mattsql::IoRequest>(&bad_flags, 1));
  EXPECT_TRUE(bad_flags_submission.status.code == mattsql::ErrorCode::InvalidArgument);

  std::span<mattsql::IoCompletion> empty_completions;
  auto empty_poll = runtime.PollIoCompletions(empty_completions);
  EXPECT_TRUE(empty_poll.status.code == mattsql::ErrorCode::InvalidArgument);

  mattsql::TaskDescriptor task;
  task.name = "not supported";
  auto task_result = runtime.SpawnTask(task);
  EXPECT_TRUE(task_result.status.code == mattsql::ErrorCode::NotSupported);
}

/// Verifies successful ABI callbacks still have to return valid v1 metadata.
TEST_CASE(c_abi_runtime_rejects_invalid_backend_success_metadata) {
  FakeAbiRuntimeState state;
  mattsql::CAbiPlatformRuntime runtime(make_fake_runtime(state));
  std::vector<std::byte> buffer(32, std::byte{0x11});

  state.return_invalid_allocation_metadata = true;
  auto allocation =
      runtime.AllocatePages(1, 8192, mattsql::kRuntimeMemoryZeroed);
  EXPECT_TRUE(allocation.status.code == mattsql::ErrorCode::Internal);
  state.return_invalid_allocation_metadata = false;

  state.return_unknown_allocation_flags = true;
  allocation = runtime.AllocatePages(1, 8192, mattsql::kRuntimeMemoryZeroed);
  EXPECT_TRUE(allocation.status.code == mattsql::ErrorCode::Internal);
  state.return_unknown_allocation_flags = false;

  state.return_non_power_allocation_alignment = true;
  allocation = runtime.AllocatePages(1, 8192, mattsql::kRuntimeMemoryZeroed);
  EXPECT_TRUE(allocation.status.code == mattsql::ErrorCode::Internal);
  state.return_non_power_allocation_alignment = false;

  mattsql::IoRequest request;
  request.operation = mattsql::IoOperation::Write;
  request.offset = 0;
  request.buffer = test::mutable_view(buffer);

  state.return_invalid_submission_count = true;
  auto submission =
      runtime.SubmitIoBatch(std::span<const mattsql::IoRequest>(&request, 1));
  EXPECT_TRUE(submission.status.code == mattsql::ErrorCode::Internal);
  state.return_invalid_submission_count = false;

  state.return_invalid_first_request_id = true;
  auto first_id_submission =
      runtime.SubmitIoBatch(std::span<const mattsql::IoRequest>(&request, 1));
  EXPECT_TRUE(first_id_submission.status.code == mattsql::ErrorCode::Internal);
  state.return_invalid_first_request_id = false;

  state.return_invalid_completion_count = true;
  std::vector<mattsql::IoCompletion> completions(1);
  auto completion_count = runtime.PollIoCompletions(
      std::span<mattsql::IoCompletion>(completions.data(), completions.size()));
  EXPECT_TRUE(completion_count.status.code == mattsql::ErrorCode::Internal);
  state.return_invalid_completion_count = false;

  state.return_zero_completion_id = true;
  completion_count = runtime.PollIoCompletions(
      std::span<mattsql::IoCompletion>(completions.data(), completions.size()));
  EXPECT_TRUE(completion_count.status.code == mattsql::ErrorCode::Internal);
  state.return_zero_completion_id = false;
}

/// Verifies optional and failing non-I/O callbacks keep their boundary semantics.
TEST_CASE(c_abi_runtime_handles_optional_and_failed_callbacks) {
  FakeAbiRuntimeState state;
  auto optional_yield_table = make_fake_runtime(state);
  optional_yield_table.yield = nullptr;
  mattsql::CAbiPlatformRuntime optional_yield_runtime(optional_yield_table);

  EXPECT_TRUE(mattsql::status_ok(optional_yield_runtime.Validate()));
  EXPECT_TRUE(mattsql::status_ok(optional_yield_runtime.Yield()));
  EXPECT_TRUE(!state.yielded);

  mattsql::CAbiPlatformRuntime runtime(make_fake_runtime(state));
  state.fail_yield = true;
  const auto yield_status = runtime.Yield();
  EXPECT_TRUE(yield_status.code == mattsql::ErrorCode::Internal);
  EXPECT_TRUE(!yield_status.message.empty());
  state.fail_yield = false;

  state.fail_clock = true;
  EXPECT_EQ(runtime.MonotonicNanos(), std::uint64_t{0});
}
