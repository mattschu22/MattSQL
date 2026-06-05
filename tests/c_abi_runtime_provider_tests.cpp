#include "mattsql/common/result_utils.hpp"
#include "mattsql/runtime/c_abi_platform_runtime.hpp"
#include "mattsql/runtime/c_abi_runtime_provider.hpp"
#include "mattsql/runtime/hosted_platform_runtime.hpp"
#include "mattsql/storage/memory_block_device.hpp"

#include "test_framework.hpp"

#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace {

mattsql::BufferView mutable_view(std::vector<std::byte> &bytes) {
  return mattsql::BufferView{std::span<std::byte>(bytes.data(), bytes.size())};
}

void expect_bytes_equal(const std::vector<std::byte> &actual,
                        const std::vector<std::byte> &expected) {
  EXPECT_EQ(actual.size(), expected.size());
  for (std::size_t index = 0; index < actual.size(); ++index) {
    EXPECT_TRUE(actual[index] == expected[index]);
  }
}

class MissingValueRuntime final : public mattsql::PlatformRuntime {
public:
  mattsql::RuntimeCapabilities GetCapabilities() const override { return {}; }

  mattsql::Result<mattsql::RuntimePageAllocation>
  AllocatePages(std::size_t page_count, std::size_t alignment,
                mattsql::RuntimeMemoryFlags flags) override {
    (void)page_count;
    (void)alignment;
    (void)flags;
    return {};
  }

  mattsql::Status FreePages(const mattsql::RuntimePageAllocation &allocation) override {
    (void)allocation;
    return mattsql::ok_status();
  }

  mattsql::Result<mattsql::IoSubmissionResult>
  SubmitIoBatch(std::span<const mattsql::IoRequest> requests) override {
    (void)requests;
    return {};
  }

  mattsql::Result<std::size_t>
  PollIoCompletions(std::span<mattsql::IoCompletion> completions) override {
    (void)completions;
    return {};
  }

  mattsql::Result<mattsql::RuntimeTaskId>
  SpawnTask(const mattsql::TaskDescriptor &descriptor) override {
    (void)descriptor;
    return mattsql::ok_result(mattsql::RuntimeTaskId{1});
  }

  mattsql::Status Yield() override { return mattsql::ok_status(); }

  std::uint64_t MonotonicNanos() const override { return 0; }

  void Log(mattsql::LogLevel level, std::string_view message) override {
    (void)level;
    (void)message;
  }

  [[noreturn]] void Panic(std::string_view message) override {
    (void)message;
    std::abort();
  }
};

class InvalidMetadataRuntime final : public mattsql::PlatformRuntime {
public:
  enum class AllocationMode { NullData, BadAlignment, UnknownFlags };
  enum class SubmissionMode { ShortCount, ZeroFirstId, MismatchedFirstId };

  mattsql::RuntimeCapabilities GetCapabilities() const override { return {}; }

  mattsql::Result<mattsql::RuntimePageAllocation>
  AllocatePages(std::size_t page_count, std::size_t alignment,
                mattsql::RuntimeMemoryFlags flags) override {
    (void)page_count;
    (void)alignment;
    (void)flags;

    mattsql::RuntimePageAllocation allocation;
    allocation.data = reinterpret_cast<void *>(std::uintptr_t{0x1000});
    allocation.page_count = 1;
    allocation.page_size = mattsql::kDefaultPageSize;
    allocation.alignment = mattsql::kDefaultPageSize;
    allocation.flags = mattsql::kRuntimeMemoryZeroed;

    switch (allocation_mode) {
    case AllocationMode::NullData:
      allocation.data = nullptr;
      break;
    case AllocationMode::BadAlignment:
      allocation.alignment = 3;
      break;
    case AllocationMode::UnknownFlags:
      allocation.flags = 1U << 12U;
      break;
    }

    return mattsql::ok_result(allocation);
  }

  mattsql::Status FreePages(const mattsql::RuntimePageAllocation &allocation) override {
    (void)allocation;
    ++free_pages_calls;
    return mattsql::ok_status();
  }

  mattsql::Result<mattsql::IoSubmissionResult>
  SubmitIoBatch(std::span<const mattsql::IoRequest> requests) override {
    mattsql::IoSubmissionResult submission;
    submission.submitted_count = requests.size();
    submission.first_request_id =
        requests.empty() || requests.front().id == 0 ? mattsql::IoRequestId{1}
                                                     : requests.front().id;

    switch (submission_mode) {
    case SubmissionMode::ShortCount:
      submission.submitted_count = 0;
      break;
    case SubmissionMode::ZeroFirstId:
      submission.first_request_id = 0;
      break;
    case SubmissionMode::MismatchedFirstId:
      submission.first_request_id = submission.first_request_id + 1U;
      break;
    }

    return mattsql::ok_result(submission);
  }

  mattsql::Result<std::size_t>
  PollIoCompletions(std::span<mattsql::IoCompletion> completions) override {
    (void)completions;
    return mattsql::ok_result(std::size_t{0});
  }

  mattsql::Result<mattsql::RuntimeTaskId>
  SpawnTask(const mattsql::TaskDescriptor &descriptor) override {
    (void)descriptor;
    return mattsql::ok_result(mattsql::RuntimeTaskId{1});
  }

  mattsql::Status Yield() override { return mattsql::ok_status(); }

  std::uint64_t MonotonicNanos() const override { return 0; }

  void Log(mattsql::LogLevel level, std::string_view message) override {
    (void)level;
    (void)message;
  }

  [[noreturn]] void Panic(std::string_view message) override {
    (void)message;
    std::abort();
  }

  AllocationMode allocation_mode = AllocationMode::NullData;
  SubmissionMode submission_mode = SubmissionMode::ShortCount;
  int free_pages_calls = 0;
};

} // namespace

/// Verifies a PlatformRuntime-backed provider exposes a valid version-1 ABI table.
TEST_CASE(c_abi_provider_exports_runtime_table_and_capabilities) {
  mattsql::MemoryBlockDevice device(128, 32);
  mattsql::HostedPlatformRuntime hosted_runtime(device);
  mattsql::CAbiRuntimeProvider provider(hosted_runtime);
  const auto table = provider.RuntimeTable();

  EXPECT_EQ(table.version, MATTSQL_ABI_RUNTIME_VERSION);
  EXPECT_TRUE(table.context != nullptr);
  EXPECT_TRUE(table.get_capabilities != nullptr);
  EXPECT_TRUE(table.allocate_pages != nullptr);
  EXPECT_TRUE(table.free_pages != nullptr);
  EXPECT_TRUE(table.submit_io_batch != nullptr);
  EXPECT_TRUE(table.poll_io_completions != nullptr);
  EXPECT_TRUE(table.yield != nullptr);
  EXPECT_TRUE(table.monotonic_nanos != nullptr);
  EXPECT_TRUE(table.log != nullptr);
  EXPECT_TRUE(table.panic != nullptr);

  mattsql_abi_runtime_capabilities capabilities{};
  auto status = table.get_capabilities(table.context, &capabilities);
  EXPECT_EQ(status.code, MATTSQL_ABI_STATUS_OK);
  EXPECT_EQ(capabilities.page_size, std::uint64_t{mattsql::kDefaultPageSize});
  EXPECT_EQ(capabilities.block_size, std::uint64_t{32});
  EXPECT_EQ(capabilities.supports_flush, std::uint8_t{1});
  EXPECT_EQ(capabilities.supports_barriers, std::uint8_t{1});

  status = table.get_capabilities(table.context, nullptr);
  EXPECT_EQ(status.code, MATTSQL_ABI_STATUS_INVALID_ARGUMENT);
  EXPECT_TRUE(status.message != nullptr);
  EXPECT_TRUE(status.message_length > 0);
}

/// Verifies the hosted runtime can round-trip through provider and consumer ABI adapters.
TEST_CASE(c_abi_provider_round_trips_hosted_runtime_through_consumer_adapter) {
  mattsql::MemoryBlockDevice device(128, 32);
  mattsql::HostedPlatformRuntime hosted_runtime(device);
  mattsql::CAbiRuntimeProvider provider(hosted_runtime);
  mattsql::CAbiPlatformRuntime loopback_runtime(provider.RuntimeTable());

  EXPECT_TRUE(mattsql::status_ok(loopback_runtime.Validate()));
  const auto capabilities = loopback_runtime.GetCapabilities();
  EXPECT_EQ(capabilities.block_size, 32U);
  EXPECT_TRUE(capabilities.supports_flush);
  EXPECT_TRUE(capabilities.supports_barriers);

  auto allocation_result =
      loopback_runtime.AllocatePages(1, 8192, mattsql::kRuntimeMemoryZeroed);
  EXPECT_TRUE(mattsql::status_ok(allocation_result.status));
  EXPECT_TRUE(allocation_result.value.has_value());
  auto allocation = *allocation_result.value;
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(allocation.data) % 8192U,
            std::uintptr_t{0});
  EXPECT_TRUE(mattsql::status_ok(loopback_runtime.FreePages(allocation)));

  std::vector<std::byte> source(32, std::byte{0x2A});
  std::vector<std::byte> output(32, std::byte{0});
  std::vector<mattsql::IoRequest> batch(2);
  batch[0].operation = mattsql::IoOperation::Write;
  batch[0].offset = 0;
  batch[0].buffer = mutable_view(source);
  batch[0].user_data = 101;
  batch[1].id = 77;
  batch[1].operation = mattsql::IoOperation::Flush;
  batch[1].offset = 0;
  batch[1].length = 32;
  batch[1].flags = mattsql::kIoRequestBarrierAfter;
  batch[1].user_data = 102;

  auto submission = loopback_runtime.SubmitIoBatch(
      std::span<const mattsql::IoRequest>(batch.data(), batch.size()));
  EXPECT_TRUE(mattsql::status_ok(submission.status));
  EXPECT_TRUE(submission.value.has_value());
  EXPECT_EQ(submission.value->submitted_count, 2U);
  EXPECT_EQ(submission.value->first_request_id, mattsql::IoRequestId{1});

  std::vector<mattsql::IoCompletion> completions(2);
  auto count = loopback_runtime.PollIoCompletions(
      std::span<mattsql::IoCompletion>(completions.data(), completions.size()));
  EXPECT_TRUE(mattsql::status_ok(count.status));
  EXPECT_TRUE(count.value.has_value());
  EXPECT_EQ(*count.value, 2U);
  EXPECT_EQ(completions[0].id, mattsql::IoRequestId{1});
  EXPECT_TRUE(mattsql::status_ok(completions[0].status));
  EXPECT_EQ(completions[0].bytes_transferred, 32U);
  EXPECT_EQ(completions[0].user_data, std::uintptr_t{101});
  EXPECT_EQ(completions[1].id, mattsql::IoRequestId{77});
  EXPECT_TRUE(mattsql::status_ok(completions[1].status));
  EXPECT_EQ(completions[1].user_data, std::uintptr_t{102});

  mattsql::IoRequest read_request;
  read_request.operation = mattsql::IoOperation::Read;
  read_request.offset = 0;
  read_request.buffer = mutable_view(output);
  auto read_request_id = loopback_runtime.SubmitIo(read_request);
  EXPECT_TRUE(mattsql::status_ok(read_request_id.status));
  auto read_completion = loopback_runtime.PollIoCompletion();
  EXPECT_TRUE(mattsql::status_ok(read_completion.status));
  EXPECT_TRUE(read_completion.value.has_value());
  EXPECT_TRUE(mattsql::status_ok(read_completion.value->status));
  EXPECT_EQ(read_completion.value->bytes_transferred, 32U);
  expect_bytes_equal(output, source);
}

/// Verifies provider-side completion errors survive the provider-consumer loopback.
TEST_CASE(c_abi_provider_preserves_completion_error_status_messages) {
  mattsql::HostedPlatformRuntime hosted_runtime;
  mattsql::CAbiRuntimeProvider provider(hosted_runtime);
  mattsql::CAbiPlatformRuntime loopback_runtime(provider.RuntimeTable());

  mattsql::IoRequest request;
  request.operation = mattsql::IoOperation::Flush;
  request.length = 32;
  auto request_id = loopback_runtime.SubmitIo(request);
  EXPECT_TRUE(mattsql::status_ok(request_id.status));

  auto completion = loopback_runtime.PollIoCompletion();
  EXPECT_TRUE(mattsql::status_ok(completion.status));
  EXPECT_TRUE(completion.value.has_value());
  EXPECT_TRUE(completion.value->status.code == mattsql::ErrorCode::NotSupported);
  EXPECT_TRUE(!completion.value->status.message.empty());
}

/// Verifies the provider rejects malformed direct C ABI requests before dispatch.
TEST_CASE(c_abi_provider_rejects_invalid_direct_abi_requests) {
  mattsql::MemoryBlockDevice device(128, 32);
  mattsql::HostedPlatformRuntime hosted_runtime(device);
  mattsql::CAbiRuntimeProvider provider(hosted_runtime);
  const auto table = provider.RuntimeTable();
  std::vector<std::byte> buffer(32);

  mattsql_abi_runtime_capabilities capabilities{};
  auto status = table.get_capabilities(nullptr, &capabilities);
  EXPECT_EQ(status.code, MATTSQL_ABI_STATUS_INVALID_ARGUMENT);

  mattsql_abi_page_allocation allocation{};
  status = table.allocate_pages(nullptr, 1, 8192, MATTSQL_ABI_MEMORY_NORMAL,
                                &allocation);
  EXPECT_EQ(status.code, MATTSQL_ABI_STATUS_INVALID_ARGUMENT);

  status = table.allocate_pages(table.context, 1, 8192,
                                MATTSQL_ABI_MEMORY_NORMAL, nullptr);
  EXPECT_EQ(status.code, MATTSQL_ABI_STATUS_INVALID_ARGUMENT);

  status = table.allocate_pages(table.context, 0, 8192,
                                MATTSQL_ABI_MEMORY_NORMAL, &allocation);
  EXPECT_EQ(status.code, MATTSQL_ABI_STATUS_INVALID_ARGUMENT);

  status = table.allocate_pages(table.context, 1, 8192, 1U << 12U, &allocation);
  EXPECT_EQ(status.code, MATTSQL_ABI_STATUS_INVALID_ARGUMENT);

  status = table.free_pages(nullptr, &allocation);
  EXPECT_EQ(status.code, MATTSQL_ABI_STATUS_INVALID_ARGUMENT);

  status = table.free_pages(table.context, nullptr);
  EXPECT_EQ(status.code, MATTSQL_ABI_STATUS_INVALID_ARGUMENT);

  status = table.free_pages(table.context, &allocation);
  EXPECT_EQ(status.code, MATTSQL_ABI_STATUS_INVALID_ARGUMENT);

  mattsql_abi_io_request request{};
  request.operation = MATTSQL_ABI_IO_WRITE;
  request.buffer = buffer.data();
  request.buffer_length = buffer.size();
  mattsql_abi_io_submission_result submission{};

  status = table.submit_io_batch(nullptr, &request, 1, &submission);
  EXPECT_EQ(status.code, MATTSQL_ABI_STATUS_INVALID_ARGUMENT);

  status = table.submit_io_batch(table.context, nullptr, 1, &submission);
  EXPECT_EQ(status.code, MATTSQL_ABI_STATUS_INVALID_ARGUMENT);

  status = table.submit_io_batch(table.context, &request, 0, &submission);
  EXPECT_EQ(status.code, MATTSQL_ABI_STATUS_INVALID_ARGUMENT);

  status = table.submit_io_batch(table.context, &request, 1, nullptr);
  EXPECT_EQ(status.code, MATTSQL_ABI_STATUS_INVALID_ARGUMENT);

  request.operation = 999U;
  status = table.submit_io_batch(table.context, &request, 1, &submission);
  EXPECT_EQ(status.code, MATTSQL_ABI_STATUS_INVALID_ARGUMENT);

  request.operation = MATTSQL_ABI_IO_WRITE;
  request.flags = 1U << 12U;
  status = table.submit_io_batch(table.context, &request, 1, &submission);
  EXPECT_EQ(status.code, MATTSQL_ABI_STATUS_INVALID_ARGUMENT);

  request = {};
  request.operation = MATTSQL_ABI_IO_WRITE;
  request.buffer = nullptr;
  request.buffer_length = buffer.size();
  status = table.submit_io_batch(table.context, &request, 1, &submission);
  EXPECT_EQ(status.code, MATTSQL_ABI_STATUS_INVALID_ARGUMENT);

  request = {};
  request.operation = MATTSQL_ABI_IO_WRITE;
  request.buffer = buffer.data();
  request.buffer_length = buffer.size();
  request.length = buffer.size() + 1U;
  status = table.submit_io_batch(table.context, &request, 1, &submission);
  EXPECT_EQ(status.code, MATTSQL_ABI_STATUS_INVALID_ARGUMENT);

  mattsql_abi_io_completion completion{};
  std::uint64_t completion_count = 0;
  status = table.poll_io_completions(nullptr, &completion, 1, &completion_count);
  EXPECT_EQ(status.code, MATTSQL_ABI_STATUS_INVALID_ARGUMENT);

  status = table.poll_io_completions(table.context, nullptr, 1,
                                     &completion_count);
  EXPECT_EQ(status.code, MATTSQL_ABI_STATUS_INVALID_ARGUMENT);

  status = table.poll_io_completions(table.context, &completion, 0,
                                     &completion_count);
  EXPECT_EQ(status.code, MATTSQL_ABI_STATUS_INVALID_ARGUMENT);

  status = table.poll_io_completions(table.context, &completion, 1, nullptr);
  EXPECT_EQ(status.code, MATTSQL_ABI_STATUS_INVALID_ARGUMENT);

  std::uint64_t nanos = 0;
  status = table.yield(nullptr);
  EXPECT_EQ(status.code, MATTSQL_ABI_STATUS_INVALID_ARGUMENT);

  status = table.monotonic_nanos(nullptr, &nanos);
  EXPECT_EQ(status.code, MATTSQL_ABI_STATUS_INVALID_ARGUMENT);

  status = table.monotonic_nanos(table.context, nullptr);
  EXPECT_EQ(status.code, MATTSQL_ABI_STATUS_INVALID_ARGUMENT);
}

/// Verifies the provider does not map runtime OK-without-value results to ABI OK.
TEST_CASE(c_abi_provider_rejects_runtime_success_without_values) {
  MissingValueRuntime runtime;
  mattsql::CAbiRuntimeProvider provider(runtime);
  const auto table = provider.RuntimeTable();

  mattsql_abi_page_allocation allocation{};
  auto status = table.allocate_pages(table.context, 1, mattsql::kDefaultPageSize,
                                    MATTSQL_ABI_MEMORY_ZEROED, &allocation);
  EXPECT_EQ(status.code, MATTSQL_ABI_STATUS_INTERNAL);

  std::vector<std::byte> buffer(32);
  mattsql_abi_io_request request{};
  request.operation = MATTSQL_ABI_IO_WRITE;
  request.buffer = buffer.data();
  request.buffer_length = buffer.size();
  mattsql_abi_io_submission_result submission{};
  status = table.submit_io_batch(table.context, &request, 1, &submission);
  EXPECT_EQ(status.code, MATTSQL_ABI_STATUS_INTERNAL);

  mattsql_abi_io_completion completion{};
  std::uint64_t completion_count = 0;
  status = table.poll_io_completions(table.context, &completion, 1,
                                     &completion_count);
  EXPECT_EQ(status.code, MATTSQL_ABI_STATUS_INTERNAL);
}

/// Verifies the provider rejects invalid success metadata from the C++ runtime.
TEST_CASE(c_abi_provider_rejects_invalid_runtime_success_metadata) {
  InvalidMetadataRuntime runtime;
  mattsql::CAbiRuntimeProvider provider(runtime);
  const auto table = provider.RuntimeTable();

  mattsql_abi_page_allocation allocation{};
  auto status = table.allocate_pages(table.context, 1, mattsql::kDefaultPageSize,
                                    MATTSQL_ABI_MEMORY_ZEROED, &allocation);
  EXPECT_EQ(status.code, MATTSQL_ABI_STATUS_INTERNAL);

  runtime.allocation_mode = InvalidMetadataRuntime::AllocationMode::BadAlignment;
  status = table.allocate_pages(table.context, 1, mattsql::kDefaultPageSize,
                                MATTSQL_ABI_MEMORY_ZEROED, &allocation);
  EXPECT_EQ(status.code, MATTSQL_ABI_STATUS_INTERNAL);

  runtime.allocation_mode = InvalidMetadataRuntime::AllocationMode::UnknownFlags;
  status = table.allocate_pages(table.context, 1, mattsql::kDefaultPageSize,
                                MATTSQL_ABI_MEMORY_ZEROED, &allocation);
  EXPECT_EQ(status.code, MATTSQL_ABI_STATUS_INTERNAL);

  mattsql_abi_page_allocation invalid_free{};
  invalid_free.page_count = 1;
  invalid_free.page_size = mattsql::kDefaultPageSize;
  invalid_free.alignment = mattsql::kDefaultPageSize;
  invalid_free.flags = MATTSQL_ABI_MEMORY_ZEROED;
  status = table.free_pages(table.context, &invalid_free);
  EXPECT_EQ(status.code, MATTSQL_ABI_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(runtime.free_pages_calls, 0);

  invalid_free.data = reinterpret_cast<void *>(std::uintptr_t{0x1000});
  invalid_free.alignment = 3;
  status = table.free_pages(table.context, &invalid_free);
  EXPECT_EQ(status.code, MATTSQL_ABI_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(runtime.free_pages_calls, 0);

  std::vector<std::byte> buffer(32);
  mattsql_abi_io_request request{};
  request.id = 77;
  request.operation = MATTSQL_ABI_IO_WRITE;
  request.buffer = buffer.data();
  request.buffer_length = buffer.size();
  mattsql_abi_io_submission_result submission{};

  runtime.submission_mode = InvalidMetadataRuntime::SubmissionMode::ShortCount;
  status = table.submit_io_batch(table.context, &request, 1, &submission);
  EXPECT_EQ(status.code, MATTSQL_ABI_STATUS_INTERNAL);

  runtime.submission_mode = InvalidMetadataRuntime::SubmissionMode::ZeroFirstId;
  status = table.submit_io_batch(table.context, &request, 1, &submission);
  EXPECT_EQ(status.code, MATTSQL_ABI_STATUS_INTERNAL);

  runtime.submission_mode = InvalidMetadataRuntime::SubmissionMode::MismatchedFirstId;
  status = table.submit_io_batch(table.context, &request, 1, &submission);
  EXPECT_EQ(status.code, MATTSQL_ABI_STATUS_INTERNAL);
}
