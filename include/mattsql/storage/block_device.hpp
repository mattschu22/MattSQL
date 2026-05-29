#pragma once

#include "mattsql/common/status.hpp"
#include "mattsql/common/types.hpp"

#include <cstddef>
#include <cstdint>

namespace mattsql {

class BlockDevice {
public:
    /// Destroys a block device through the interface pointer.
    virtual ~BlockDevice() = default;

    /// Returns the device's logical block size in bytes.
    virtual std::size_t BlockSize() const = 0;

    /// Returns the device size in bytes.
    virtual std::uint64_t SizeBytes() const = 0;

    /// Reads bytes from the device into a mutable buffer.
    virtual Status Read(StorageOffset offset, BufferView destination) = 0;

    /// Writes bytes from a read-only buffer to the device.
    virtual Status Write(StorageOffset offset, ConstBufferView source) = 0;

    /// Flushes a byte range to durable media.
    virtual Status Flush(StorageOffset offset, std::size_t length) = 0;
};

} // namespace mattsql
