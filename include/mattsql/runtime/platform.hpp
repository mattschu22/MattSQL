#pragma once

#include "mattsql/common/status.hpp"
#include "mattsql/common/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
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

using RuntimeMemoryFlags = std::uint32_t;

inline constexpr RuntimeMemoryFlags kRuntimeMemoryNormal = 0;
inline constexpr RuntimeMemoryFlags kRuntimeMemoryZeroed = 1U << 0U;
inline constexpr RuntimeMemoryFlags kRuntimeMemoryDma = 1U << 1U;

using IoRequestFlags = std::uint32_t;

inline constexpr IoRequestFlags kIoRequestNoFlags = 0;
inline constexpr IoRequestFlags kIoRequestBarrierBefore = 1U << 0U;
inline constexpr IoRequestFlags kIoRequestBarrierAfter = 1U << 1U;
inline constexpr IoRequestFlags kIoRequestForceUnitAccess = 1U << 2U;

struct RuntimeCapabilities {
    std::size_t page_size = kDefaultPageSize;
    std::size_t block_size = 0;
    std::size_t max_io_request_size = 0;
    std::size_t max_io_batch_size = 1;
    std::size_t max_outstanding_io = 1;
    bool supports_async_io = false;
    bool supports_flush = false;
    bool supports_barriers = false;
    bool supports_physical_addresses = false;
    bool supports_dma_memory = false;
};

struct RuntimePageAllocation {
    void* data = nullptr;
    std::size_t page_count = 0;
    std::size_t page_size = kDefaultPageSize;
    std::size_t alignment = kDefaultPageSize;
    RuntimeMemoryFlags flags = kRuntimeMemoryZeroed;
    std::uint64_t physical_address = 0;
};

struct IoRequest {
    IoRequestId id = 0;
    IoOperation operation = IoOperation::Read;
    StorageOffset offset = 0;
    BufferView buffer;
    std::size_t length = 0;
    IoRequestFlags flags = kIoRequestNoFlags;
    std::uintptr_t user_data = 0;
};

struct IoCompletion {
    IoRequestId id = 0;
    Status status;
    std::size_t bytes_transferred = 0;
    std::uintptr_t user_data = 0;
};

struct IoSubmissionResult {
    std::size_t submitted_count = 0;
    IoRequestId first_request_id = 0;
};

struct TaskDescriptor {
    RuntimeTaskId id = 0;
    std::string_view name;
    std::uint32_t preferred_core = 0;
};

class PlatformRuntime {
public:
    /// Destroys a platform runtime through the interface pointer.
    virtual ~PlatformRuntime();

    /// Returns the runtime services and limits available through this backend.
    virtual RuntimeCapabilities GetCapabilities() const = 0;

    /// Allocates runtime-managed page spans for database structures or I/O buffers.
    virtual Result<RuntimePageAllocation>
    AllocatePages(std::size_t page_count, std::size_t alignment,
                  RuntimeMemoryFlags flags) = 0;

    /// Allocates runtime-managed pages for database structures or I/O buffers.
    Result<RuntimePageAllocation> AllocatePages(std::size_t page_count);

    /// Releases pages previously returned by AllocatePages.
    virtual Status FreePages(const RuntimePageAllocation& allocation) = 0;

    /// Submits one or more I/O requests to the platform's block I/O queue.
    virtual Result<IoSubmissionResult>
    SubmitIoBatch(std::span<const IoRequest> requests) = 0;

    /// Submits an asynchronous I/O request to the platform.
    Result<IoRequestId> SubmitIo(const IoRequest& request);

    /// Polls for up to completions.size() completed I/O requests.
    virtual Result<std::size_t>
    PollIoCompletions(std::span<IoCompletion> completions) = 0;

    /// Polls for the next completed I/O request.
    Result<IoCompletion> PollIoCompletion();

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
