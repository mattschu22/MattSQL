#include "mattsql/common/result_utils.hpp"
#include "mattsql/runtime/hosted_platform_runtime.hpp"
#include "mattsql/storage/memory_block_device.hpp"

#include "mattsql_test_utils.hpp"
#include "test_framework.hpp"

#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace {

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

class MalformedBatchRuntime final : public mattsql::PlatformRuntime {
public:
  mattsql::RuntimeCapabilities GetCapabilities() const override { return {}; }

  mattsql::Result<mattsql::RuntimePageAllocation>
  AllocatePages(std::size_t page_count, std::size_t alignment,
                mattsql::RuntimeMemoryFlags flags) override {
    (void)flags;
    mattsql::RuntimePageAllocation allocation;
    allocation.data = reinterpret_cast<void *>(std::uintptr_t{0x1000});
    allocation.page_count = page_count;
    allocation.page_size = mattsql::kDefaultPageSize;
    allocation.alignment = alignment;
    return mattsql::ok_result(allocation);
  }

  mattsql::Status FreePages(const mattsql::RuntimePageAllocation &allocation) override {
    (void)allocation;
    return mattsql::ok_status();
  }

  mattsql::Result<mattsql::IoSubmissionResult>
  SubmitIoBatch(std::span<const mattsql::IoRequest> requests) override {
    (void)requests;
    mattsql::IoSubmissionResult submission;
    submission.submitted_count = submitted_count;
    submission.first_request_id = first_request_id;
    return mattsql::ok_result(submission);
  }

  mattsql::Result<std::size_t>
  PollIoCompletions(std::span<mattsql::IoCompletion> completions) override {
    if (!completions.empty()) {
      completions[0].id = mattsql::IoRequestId{7};
      completions[0].status = mattsql::ok_status();
    }
    return mattsql::ok_result(completion_count);
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

  std::size_t submitted_count = 1;
  mattsql::IoRequestId first_request_id = 1;
  std::size_t completion_count = 1;
};

} // namespace

/// Verifies the hosted runtime reports the minimal scalable runtime boundary.
TEST_CASE(hosted_runtime_reports_minimum_capabilities) {
  mattsql::HostedPlatformRuntime runtime;
  const auto capabilities = runtime.GetCapabilities();

  EXPECT_EQ(capabilities.page_size, mattsql::kDefaultPageSize);
  EXPECT_EQ(capabilities.block_size, 0U);
  EXPECT_TRUE(capabilities.max_io_request_size >= mattsql::kDefaultPageSize);
  EXPECT_TRUE(capabilities.max_io_batch_size >= 1U);
  EXPECT_TRUE(capabilities.max_outstanding_io >= capabilities.max_io_batch_size);
  EXPECT_TRUE(!capabilities.supports_async_io);
  EXPECT_TRUE(!capabilities.supports_flush);
  EXPECT_TRUE(!capabilities.supports_barriers);
  EXPECT_TRUE(!capabilities.supports_physical_addresses);
  EXPECT_TRUE(!capabilities.supports_dma_memory);

  mattsql::MemoryBlockDevice device(128, 32);
  mattsql::HostedPlatformRuntime block_runtime(device);
  const auto block_capabilities = block_runtime.GetCapabilities();

  EXPECT_EQ(block_capabilities.block_size, 32U);
  EXPECT_TRUE(block_capabilities.supports_flush);
  EXPECT_TRUE(block_capabilities.supports_barriers);
}

/// Verifies runtime page allocation is span-shaped, aligned, and explicitly freed.
TEST_CASE(hosted_runtime_allocates_aligned_zeroed_page_spans) {
  mattsql::HostedPlatformRuntime runtime;
  constexpr std::size_t kAlignment = 8192;

  auto default_allocation_result = runtime.AllocatePages(1);
  EXPECT_TRUE(mattsql::status_ok(default_allocation_result.status));
  EXPECT_TRUE(default_allocation_result.value.has_value());
  EXPECT_TRUE(mattsql::status_ok(runtime.FreePages(*default_allocation_result.value)));

  auto allocation_result =
      runtime.AllocatePages(2, kAlignment, mattsql::kRuntimeMemoryZeroed);
  EXPECT_TRUE(mattsql::status_ok(allocation_result.status));
  EXPECT_TRUE(allocation_result.value.has_value());

  auto allocation = *allocation_result.value;
  EXPECT_TRUE(allocation.data != nullptr);
  EXPECT_EQ(allocation.page_count, 2U);
  EXPECT_EQ(allocation.page_size, mattsql::kDefaultPageSize);
  EXPECT_EQ(allocation.alignment, kAlignment);
  EXPECT_EQ(allocation.flags, mattsql::kRuntimeMemoryZeroed);
  EXPECT_EQ(allocation.physical_address, std::uint64_t{0});
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(allocation.data) % kAlignment,
            std::uintptr_t{0});

  auto *bytes = static_cast<std::byte *>(allocation.data);
  for (std::size_t index = 0; index < allocation.page_count * allocation.page_size;
       ++index) {
    EXPECT_TRUE(bytes[index] == std::byte{0});
  }

  EXPECT_TRUE(mattsql::status_ok(runtime.FreePages(allocation)));
}

/// Verifies runtime page allocations can be owned with RAII move semantics.
TEST_CASE(hosted_runtime_page_span_owner_releases_allocations) {
  mattsql::HostedPlatformRuntime runtime;

  auto page_span = runtime.AllocatePageSpan(1);
  EXPECT_TRUE(mattsql::status_ok(page_span.status));
  EXPECT_TRUE(page_span.value.has_value());
  EXPECT_TRUE(*page_span.value);
  EXPECT_TRUE(page_span.value->data() != nullptr);
  EXPECT_EQ(page_span.value->bytes().size(), mattsql::kDefaultPageSize);

  mattsql::RuntimePageAllocationHandle moved = std::move(*page_span.value);
  EXPECT_TRUE(moved);
  EXPECT_TRUE(!*page_span.value);

  const auto raw = moved.Release();
  EXPECT_TRUE(!moved);
  EXPECT_TRUE(raw.data != nullptr);
  EXPECT_TRUE(mattsql::status_ok(runtime.FreePages(raw)));
}

/// Verifies invalid page-span requests fail before allocation.
TEST_CASE(hosted_runtime_rejects_invalid_page_span_requests) {
  mattsql::HostedPlatformRuntime runtime;

  auto zero_pages = runtime.AllocatePages(0, mattsql::kDefaultPageSize,
                                          mattsql::kRuntimeMemoryZeroed);
  EXPECT_TRUE(zero_pages.status.code == mattsql::ErrorCode::InvalidArgument);

  auto bad_alignment = runtime.AllocatePages(1, 3, mattsql::kRuntimeMemoryZeroed);
  EXPECT_TRUE(bad_alignment.status.code == mattsql::ErrorCode::InvalidArgument);

  auto dma_memory =
      runtime.AllocatePages(1, mattsql::kDefaultPageSize, mattsql::kRuntimeMemoryDma);
  EXPECT_TRUE(dma_memory.status.code == mattsql::ErrorCode::NotSupported);

  mattsql::RuntimePageAllocation missing_data;
  EXPECT_TRUE(runtime.FreePages(missing_data).code ==
              mattsql::ErrorCode::InvalidArgument);
}

/// Verifies batch-shaped I/O executes synchronously against an attached block device.
TEST_CASE(hosted_runtime_submits_and_polls_batched_block_io) {
  mattsql::MemoryBlockDevice device(128, 32);
  mattsql::HostedPlatformRuntime runtime(device);
  std::vector<std::byte> source(32, std::byte{0x5A});
  std::vector<std::byte> output(32, std::byte{0});

  std::vector<mattsql::IoRequest> write_batch(2);
  write_batch[0].id = 7;
  write_batch[0].operation = mattsql::IoOperation::Write;
  write_batch[0].offset = 0;
  write_batch[0].buffer = test::mutable_view(source);
  write_batch[0].flags = mattsql::kIoRequestBarrierBefore;
  write_batch[0].user_data = 17;
  write_batch[1].id = 8;
  write_batch[1].operation = mattsql::IoOperation::Flush;
  write_batch[1].offset = 0;
  write_batch[1].length = 32;
  write_batch[1].flags = mattsql::kIoRequestBarrierAfter;
  write_batch[1].user_data = 18;

  auto submission = runtime.SubmitIoBatch(
      std::span<const mattsql::IoRequest>(write_batch.data(), write_batch.size()));
  EXPECT_TRUE(mattsql::status_ok(submission.status));
  EXPECT_TRUE(submission.value.has_value());
  EXPECT_EQ(submission.value->submitted_count, 2U);
  EXPECT_EQ(submission.value->first_request_id, mattsql::IoRequestId{7});

  std::vector<mattsql::IoCompletion> completions(4);
  auto completion_count = runtime.PollIoCompletions(
      std::span<mattsql::IoCompletion>(completions.data(), completions.size()));
  EXPECT_TRUE(mattsql::status_ok(completion_count.status));
  EXPECT_TRUE(completion_count.value.has_value());
  EXPECT_EQ(*completion_count.value, 2U);
  EXPECT_EQ(completions[0].id, mattsql::IoRequestId{7});
  EXPECT_TRUE(mattsql::status_ok(completions[0].status));
  EXPECT_EQ(completions[0].bytes_transferred, 32U);
  EXPECT_EQ(completions[0].user_data, std::uintptr_t{17});
  EXPECT_EQ(completions[1].id, mattsql::IoRequestId{8});
  EXPECT_TRUE(mattsql::status_ok(completions[1].status));
  EXPECT_EQ(completions[1].bytes_transferred, 0U);
  EXPECT_EQ(completions[1].user_data, std::uintptr_t{18});

  mattsql::IoRequest read_request;
  read_request.id = 9;
  read_request.operation = mattsql::IoOperation::Read;
  read_request.offset = 0;
  read_request.buffer = test::mutable_view(output);

  auto read_submission =
      runtime.SubmitIoBatch(std::span<const mattsql::IoRequest>(&read_request, 1));
  EXPECT_TRUE(mattsql::status_ok(read_submission.status));
  EXPECT_TRUE(read_submission.value.has_value());
  EXPECT_EQ(read_submission.value->submitted_count, 1U);
  EXPECT_EQ(read_submission.value->first_request_id, mattsql::IoRequestId{9});

  auto read_completion = runtime.PollIoCompletion();
  EXPECT_TRUE(mattsql::status_ok(read_completion.status));
  EXPECT_TRUE(read_completion.value.has_value());
  EXPECT_EQ(read_completion.value->id, mattsql::IoRequestId{9});
  EXPECT_EQ(read_completion.value->bytes_transferred, 32U);
  test::expect_bytes_equal(output, source);
}

/// Verifies legacy single-request helpers route through the batch boundary.
TEST_CASE(hosted_runtime_single_io_helpers_use_batch_boundary) {
  mattsql::HostedPlatformRuntime runtime;
  mattsql::IoRequest request;
  request.operation = mattsql::IoOperation::Flush;

  auto request_id = runtime.SubmitIo(request);
  EXPECT_TRUE(mattsql::status_ok(request_id.status));
  EXPECT_TRUE(request_id.value.has_value());
  EXPECT_EQ(*request_id.value, mattsql::IoRequestId{1});

  auto completion = runtime.PollIoCompletion();
  EXPECT_TRUE(mattsql::status_ok(completion.status));
  EXPECT_TRUE(completion.value.has_value());
  EXPECT_EQ(completion.value->id, mattsql::IoRequestId{1});
  EXPECT_TRUE(completion.value->status.code == mattsql::ErrorCode::NotSupported);

  auto empty_completion = runtime.PollIoCompletion();
  EXPECT_TRUE(empty_completion.status.code == mattsql::ErrorCode::NotFound);
}

/// Verifies PlatformRuntime helpers reject malformed successful batch results.
TEST_CASE(platform_runtime_helpers_reject_success_without_values) {
  MissingValueRuntime runtime;

  auto page_span = runtime.AllocatePageSpan(1);
  EXPECT_TRUE(page_span.status.code == mattsql::ErrorCode::Internal);

  mattsql::IoRequest request;
  request.operation = mattsql::IoOperation::Flush;
  auto request_id = runtime.SubmitIo(request);
  EXPECT_TRUE(request_id.status.code == mattsql::ErrorCode::Internal);

  auto completion = runtime.PollIoCompletion();
  EXPECT_TRUE(completion.status.code == mattsql::ErrorCode::Internal);
}

/// Verifies single-request helpers validate successful batch metadata.
TEST_CASE(platform_runtime_helpers_reject_malformed_success_metadata) {
  MalformedBatchRuntime runtime;

  mattsql::IoRequest request;
  request.id = mattsql::IoRequestId{9};
  request.operation = mattsql::IoOperation::Flush;

  runtime.submitted_count = 0;
  auto request_id = runtime.SubmitIo(request);
  EXPECT_TRUE(request_id.status.code == mattsql::ErrorCode::Internal);

  runtime.submitted_count = 2;
  request_id = runtime.SubmitIo(request);
  EXPECT_TRUE(request_id.status.code == mattsql::ErrorCode::Internal);

  runtime.submitted_count = 1;
  runtime.first_request_id = mattsql::IoRequestId{8};
  request_id = runtime.SubmitIo(request);
  EXPECT_TRUE(request_id.status.code == mattsql::ErrorCode::Internal);

  runtime.completion_count = 2;
  auto completion = runtime.PollIoCompletion();
  EXPECT_TRUE(completion.status.code == mattsql::ErrorCode::Internal);

  mattsql::RuntimePageAllocation allocation;
  allocation.data = reinterpret_cast<void *>(std::uintptr_t{0x1000});
  allocation.page_count = std::numeric_limits<std::size_t>::max();
  allocation.page_size = 2;
  mattsql::RuntimePageAllocationHandle handle(runtime, allocation);
  EXPECT_EQ(handle.bytes().size(), 0U);
  (void)handle.Release();
}

/// Verifies hosted I/O rejects unknown flags and operation enum values.
TEST_CASE(hosted_runtime_rejects_unknown_io_flags_and_operations) {
  mattsql::MemoryBlockDevice device(128, 32);
  mattsql::HostedPlatformRuntime runtime(device);
  std::vector<std::byte> buffer(32);

  mattsql::IoRequest request;
  request.operation = mattsql::IoOperation::Write;
  request.buffer = test::mutable_view(buffer);
  request.flags = 1U << 12U;

  auto submission =
      runtime.SubmitIoBatch(std::span<const mattsql::IoRequest>(&request, 1));
  EXPECT_TRUE(submission.status.code == mattsql::ErrorCode::InvalidArgument);

  request.flags = mattsql::kIoRequestNoFlags;
  request.operation = static_cast<mattsql::IoOperation>(999);
  submission = runtime.SubmitIoBatch(
      std::span<const mattsql::IoRequest>(&request, 1));
  EXPECT_TRUE(submission.status.code == mattsql::ErrorCode::InvalidArgument);

  mattsql::IoCompletion completion;
  auto completion_count =
      runtime.PollIoCompletions(std::span<mattsql::IoCompletion>(&completion, 1));
  EXPECT_TRUE(mattsql::status_ok(completion_count.status));
  EXPECT_EQ(*completion_count.value, 0U);
}
