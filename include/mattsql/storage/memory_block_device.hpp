#pragma once

#include "mattsql/storage/block_device.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mattsql {

class MemoryBlockDevice final : public BlockDevice {
public:
  /// Creates an in-memory block device with fixed block-aligned access.
  explicit MemoryBlockDevice(std::uint64_t size_bytes,
                             std::size_t block_size = kDefaultPageSize);

  /// Returns true when the constructor parameters describe a usable device.
  [[nodiscard]] bool IsValid() const;

  /// Returns the device's logical block size in bytes.
  std::size_t BlockSize() const override;

  /// Returns the device size in bytes.
  std::uint64_t SizeBytes() const override;

  /// Reads one or more aligned blocks into a caller-owned buffer.
  Status Read(StorageOffset offset, BufferView destination) override;

  /// Writes one or more aligned blocks from a caller-owned buffer.
  Status Write(StorageOffset offset, ConstBufferView source) override;

  /// Validates an aligned flush range. Memory-backed flushes are otherwise no-ops.
  Status Flush(StorageOffset offset, std::size_t length) override;

private:
  [[nodiscard]] Status ValidateAccess(StorageOffset offset, std::size_t length) const;

  std::size_t block_size_ = 0;
  bool valid_ = false;
  std::vector<std::byte> data_;
};

} // namespace mattsql
