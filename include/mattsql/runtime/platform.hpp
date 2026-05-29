#pragma once

#include "mattsql/common/status.hpp"
#include "mattsql/common/types.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mattsql {

enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Fatal
};

enum class IoOperation {
    Read,
    Write,
    Flush
};

struct RuntimePageAllocation {
    void* data = nullptr;
    std::size_t page_count = 0;
    std::size_t page_size = kDefaultPageSize;
    std::uint64_t physical_address = 0;
};

struct IoRequest {
    IoRequestId id = 0;
    IoOperation operation = IoOperation::Read;
    StorageOffset offset = 0;
    BufferView buffer;
};

struct IoCompletion {
    IoRequestId id = 0;
    Status status;
    std::size_t bytes_transferred = 0;
};

struct TaskDescriptor {
    RuntimeTaskId id = 0;
    std::string_view name;
    std::uint32_t preferred_core = 0;
};

class PlatformRuntime {
public:
    /// Destroys a platform runtime through the interface pointer.
    virtual ~PlatformRuntime() = default;

    /// Allocates runtime-managed pages for database structures or I/O buffers.
    virtual Result<RuntimePageAllocation> AllocatePages(std::size_t page_count) = 0;

    /// Releases pages previously returned by AllocatePages.
    virtual Status FreePages(const RuntimePageAllocation& allocation) = 0;

    /// Submits an asynchronous I/O request to the platform.
    virtual Result<IoRequestId> SubmitIo(const IoRequest& request) = 0;

    /// Polls for the next completed I/O request.
    virtual Result<IoCompletion> PollIoCompletion() = 0;

    /// Creates a runtime task known to the scheduler.
    virtual Result<RuntimeTaskId> SpawnTask(const TaskDescriptor& descriptor) = 0;

    /// Yields execution back to the scheduler.
    virtual Status Yield() = 0;

    /// Returns monotonic runtime time in nanoseconds.
    virtual std::uint64_t MonotonicNanos() const = 0;

    /// Emits a structured diagnostic log message.
    virtual void Log(LogLevel level, std::string_view message) = 0;

    /// Aborts the runtime with structured debug information.
    [[noreturn]] virtual void Panic(std::string_view message) = 0;
};

} // namespace mattsql
