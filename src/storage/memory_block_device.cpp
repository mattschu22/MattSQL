#include "mattsql/storage/memory_block_device.hpp"

#include "mattsql/common/result_utils.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace mattsql {

MemoryBlockDevice::MemoryBlockDevice(std::uint64_t size_bytes, std::size_t block_size)
    : block_size_(block_size) {
  if (block_size == 0 || size_bytes == 0 ||
      size_bytes % static_cast<std::uint64_t>(block_size) != 0 ||
      size_bytes > std::numeric_limits<std::size_t>::max()) {
    return;
  }

  data_.resize(static_cast<std::size_t>(size_bytes), std::byte{0});
  valid_ = true;
}

bool MemoryBlockDevice::IsValid() const { return valid_; }

std::size_t MemoryBlockDevice::BlockSize() const { return block_size_; }

std::uint64_t MemoryBlockDevice::SizeBytes() const {
  return static_cast<std::uint64_t>(data_.size());
}

Status MemoryBlockDevice::Read(StorageOffset offset, BufferView destination) {
  const auto status = ValidateAccess(offset, destination.bytes.size());
  if (!status_ok(status)) {
    return status;
  }

  const auto begin = data_.begin() + static_cast<std::ptrdiff_t>(offset);
  std::copy(begin, begin + static_cast<std::ptrdiff_t>(destination.bytes.size()),
            destination.bytes.begin());
  return ok_status();
}

Status MemoryBlockDevice::Write(StorageOffset offset, ConstBufferView source) {
  const auto status = ValidateAccess(offset, source.bytes.size());
  if (!status_ok(status)) {
    return status;
  }

  const auto begin = data_.begin() + static_cast<std::ptrdiff_t>(offset);
  std::copy(source.bytes.begin(), source.bytes.end(), begin);
  return ok_status();
}

Status MemoryBlockDevice::Flush(StorageOffset offset, std::size_t length) {
  return ValidateAccess(offset, length);
}

Status MemoryBlockDevice::ValidateAccess(StorageOffset offset,
                                         std::size_t length) const {
  if (!valid_) {
    return error_status(ErrorCode::InvalidArgument,
                        "memory block device has invalid geometry");
  }
  if (offset % static_cast<std::uint64_t>(block_size_) != 0 ||
      length % block_size_ != 0) {
    return error_status(ErrorCode::InvalidArgument,
                        "memory block device access must be block aligned");
  }

  const auto size_bytes = SizeBytes();
  if (offset > size_bytes) {
    return error_status(ErrorCode::InvalidArgument,
                        "memory block device access starts past the device");
  }

  const auto remaining = size_bytes - offset;
  if (static_cast<std::uint64_t>(length) > remaining) {
    return error_status(ErrorCode::InvalidArgument,
                        "memory block device access extends past the device");
  }

  return ok_status();
}

} // namespace mattsql
