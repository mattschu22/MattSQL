#include "mattsql/common/result_utils.hpp"
#include "mattsql/storage/memory_block_device.hpp"

#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {

std::vector<std::byte> filled_block(std::size_t size, unsigned char value) {
  return std::vector<std::byte>(size, static_cast<std::byte>(value));
}

mattsql::ConstBufferView const_view(const std::vector<std::byte> &bytes) {
  return mattsql::ConstBufferView{
      std::span<const std::byte>(bytes.data(), bytes.size())};
}

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

/// Verifies memory block devices expose their fixed geometry and zero-fill data.
TEST_CASE(memory_block_device_reports_geometry_and_reads_zeroes) {
  mattsql::MemoryBlockDevice device(128, 32);
  std::vector<std::byte> output(32, std::byte{0x7F});

  EXPECT_TRUE(device.IsValid());
  EXPECT_EQ(device.BlockSize(), 32U);
  EXPECT_EQ(device.SizeBytes(), std::uint64_t{128});
  EXPECT_TRUE(mattsql::status_ok(device.Read(0, mutable_view(output))));
  expect_bytes_equal(output, filled_block(32, 0));
}

/// Verifies aligned writes are durable for later reads and do not affect other blocks.
TEST_CASE(memory_block_device_writes_and_reads_aligned_blocks) {
  mattsql::MemoryBlockDevice device(128, 32);
  const auto first = filled_block(32, 0x11);
  const auto second = filled_block(32, 0x22);

  EXPECT_TRUE(mattsql::status_ok(device.Write(0, const_view(first))));
  EXPECT_TRUE(mattsql::status_ok(device.Write(32, const_view(second))));
  EXPECT_TRUE(mattsql::status_ok(device.Flush(0, 64)));

  std::vector<std::byte> output(64);
  EXPECT_TRUE(mattsql::status_ok(device.Read(0, mutable_view(output))));

  std::vector<std::byte> expected;
  expected.insert(expected.end(), first.begin(), first.end());
  expected.insert(expected.end(), second.begin(), second.end());
  expect_bytes_equal(output, expected);
}

/// Verifies misaligned and out-of-range accesses fail without changing data.
TEST_CASE(memory_block_device_rejects_invalid_accesses) {
  mattsql::MemoryBlockDevice device(128, 32);
  const auto original = filled_block(32, 0x33);
  const auto replacement = filled_block(32, 0x44);
  std::vector<std::byte> output(32);
  std::vector<std::byte> short_buffer(31);
  std::vector<std::byte> long_buffer(64);

  EXPECT_TRUE(mattsql::status_ok(device.Write(0, const_view(original))));

  EXPECT_TRUE(device.Read(1, mutable_view(output)).code ==
              mattsql::ErrorCode::InvalidArgument);
  EXPECT_TRUE(device.Read(0, mutable_view(short_buffer)).code ==
              mattsql::ErrorCode::InvalidArgument);
  EXPECT_TRUE(device.Read(96, mutable_view(long_buffer)).code ==
              mattsql::ErrorCode::InvalidArgument);
  EXPECT_TRUE(device.Write(1, const_view(replacement)).code ==
              mattsql::ErrorCode::InvalidArgument);
  EXPECT_TRUE(device.Write(0, const_view(short_buffer)).code ==
              mattsql::ErrorCode::InvalidArgument);
  EXPECT_TRUE(device.Flush(0, 31).code == mattsql::ErrorCode::InvalidArgument);
  EXPECT_TRUE(device.Flush(160, 0).code == mattsql::ErrorCode::InvalidArgument);

  EXPECT_TRUE(mattsql::status_ok(device.Read(0, mutable_view(output))));
  expect_bytes_equal(output, original);
}

/// Verifies invalid constructor geometry produces a device that refuses I/O.
TEST_CASE(memory_block_device_rejects_invalid_geometry) {
  mattsql::MemoryBlockDevice zero_size(0, 32);
  mattsql::MemoryBlockDevice zero_block(128, 0);
  mattsql::MemoryBlockDevice uneven_size(100, 32);
  std::vector<std::byte> buffer(32);

  EXPECT_TRUE(!zero_size.IsValid());
  EXPECT_EQ(zero_size.SizeBytes(), std::uint64_t{0});
  EXPECT_TRUE(zero_size.Read(0, mutable_view(buffer)).code ==
              mattsql::ErrorCode::InvalidArgument);

  EXPECT_TRUE(!zero_block.IsValid());
  EXPECT_EQ(zero_block.BlockSize(), 0U);
  EXPECT_TRUE(zero_block.Write(0, const_view(buffer)).code ==
              mattsql::ErrorCode::InvalidArgument);

  EXPECT_TRUE(!uneven_size.IsValid());
  EXPECT_EQ(uneven_size.SizeBytes(), std::uint64_t{0});
  EXPECT_TRUE(uneven_size.Flush(0, 32).code == mattsql::ErrorCode::InvalidArgument);
}
