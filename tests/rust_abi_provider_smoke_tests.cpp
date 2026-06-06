#include "mattsql/common/result_utils.hpp"
#include "mattsql/runtime/c_abi_platform_runtime.hpp"

#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

extern "C" {
mattsql_abi_runtime_v1 mattsql_rust_abi_smoke_runtime();
void mattsql_rust_abi_smoke_reset();
}

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

} // namespace

/// Verifies a Rust-exported ABI table can drive the C++ consumer adapter.
TEST_CASE(rust_abi_provider_smoke_round_trips_through_cpp_consumer) {
  mattsql_rust_abi_smoke_reset();
  mattsql::CAbiPlatformRuntime runtime(mattsql_rust_abi_smoke_runtime());

  EXPECT_TRUE(mattsql::status_ok(runtime.Validate()));
  const auto capabilities = runtime.GetCapabilities();
  EXPECT_EQ(capabilities.page_size, mattsql::kDefaultPageSize);
  EXPECT_EQ(capabilities.block_size, 32U);
  EXPECT_EQ(capabilities.max_io_batch_size, 4U);
  EXPECT_TRUE(capabilities.supports_flush);
  EXPECT_TRUE(capabilities.supports_barriers);
  EXPECT_TRUE(!capabilities.supports_dma_memory);

  auto allocation_result =
      runtime.AllocatePages(1, 8192, mattsql::kRuntimeMemoryZeroed);
  EXPECT_TRUE(mattsql::status_ok(allocation_result.status));
  EXPECT_TRUE(allocation_result.value.has_value());
  auto allocation = *allocation_result.value;
  EXPECT_TRUE(allocation.data != nullptr);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(allocation.data) % 8192U,
            std::uintptr_t{0});
  auto *allocation_bytes = static_cast<std::byte *>(allocation.data);
  EXPECT_TRUE(allocation_bytes[0] == std::byte{0});
  EXPECT_TRUE(mattsql::status_ok(runtime.FreePages(allocation)));

  auto dma_allocation =
      runtime.AllocatePages(1, 8192, mattsql::kRuntimeMemoryDma);
  EXPECT_TRUE(dma_allocation.status.code == mattsql::ErrorCode::NotSupported);

  std::vector<std::byte> source(32, std::byte{0x5A});
  std::vector<std::byte> output(32, std::byte{0});
  std::vector<mattsql::IoRequest> write_batch(2);
  write_batch[0].operation = mattsql::IoOperation::Write;
  write_batch[0].offset = 0;
  write_batch[0].buffer = mutable_view(source);
  write_batch[0].user_data = 11;
  write_batch[1].id = 77;
  write_batch[1].operation = mattsql::IoOperation::Flush;
  write_batch[1].offset = 0;
  write_batch[1].flags = mattsql::kIoRequestBarrierAfter;
  write_batch[1].user_data = 12;

  auto write_submission = runtime.SubmitIoBatch(
      std::span<const mattsql::IoRequest>(write_batch.data(),
                                          write_batch.size()));
  EXPECT_TRUE(mattsql::status_ok(write_submission.status));
  EXPECT_TRUE(write_submission.value.has_value());
  EXPECT_EQ(write_submission.value->submitted_count, 2U);
  EXPECT_EQ(write_submission.value->first_request_id, mattsql::IoRequestId{1});

  std::vector<mattsql::IoCompletion> write_completions(2);
  auto write_completion_count = runtime.PollIoCompletions(
      std::span<mattsql::IoCompletion>(write_completions.data(),
                                       write_completions.size()));
  EXPECT_TRUE(mattsql::status_ok(write_completion_count.status));
  EXPECT_TRUE(write_completion_count.value.has_value());
  EXPECT_EQ(*write_completion_count.value, 2U);
  EXPECT_EQ(write_completions[0].id, mattsql::IoRequestId{1});
  EXPECT_TRUE(mattsql::status_ok(write_completions[0].status));
  EXPECT_EQ(write_completions[0].bytes_transferred, 32U);
  EXPECT_EQ(write_completions[0].user_data, std::uintptr_t{11});
  EXPECT_EQ(write_completions[1].id, mattsql::IoRequestId{77});
  EXPECT_TRUE(mattsql::status_ok(write_completions[1].status));
  EXPECT_EQ(write_completions[1].bytes_transferred, 0U);
  EXPECT_EQ(write_completions[1].user_data, std::uintptr_t{12});

  mattsql::IoRequest read_request;
  read_request.operation = mattsql::IoOperation::Read;
  read_request.offset = 0;
  read_request.buffer = mutable_view(output);
  auto read_submission = runtime.SubmitIo(read_request);
  EXPECT_TRUE(mattsql::status_ok(read_submission.status));
  EXPECT_TRUE(read_submission.value.has_value());

  auto read_completion = runtime.PollIoCompletion();
  EXPECT_TRUE(mattsql::status_ok(read_completion.status));
  EXPECT_TRUE(read_completion.value.has_value());
  EXPECT_TRUE(mattsql::status_ok(read_completion.value->status));
  EXPECT_EQ(read_completion.value->bytes_transferred, 32U);
  expect_bytes_equal(output, source);

  const auto first_tick = runtime.MonotonicNanos();
  const auto second_tick = runtime.MonotonicNanos();
  EXPECT_TRUE(second_tick > first_tick);
  EXPECT_TRUE(mattsql::status_ok(runtime.Yield()));
  runtime.Log(mattsql::LogLevel::Warn, "rust ABI smoke");
}

int main() { return test::Registry::instance().run(); }
