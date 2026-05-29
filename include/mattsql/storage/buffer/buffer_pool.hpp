#pragma once

#include "mattsql/common/status.hpp"
#include "mattsql/common/types.hpp"
#include "mattsql/storage/page/page.hpp"

#include <memory>

namespace mattsql {

enum class PageAccessMode {
    Read,
    Write
};

class PageHandle {
public:
    /// Releases a page pin through RAII when the handle is destroyed.
    virtual ~PageHandle() = default;

    /// Returns the pinned page identifier.
    virtual PageId Id() const = 0;

    /// Returns mutable page bytes for write pins.
    virtual PageView MutablePage() = 0;

    /// Returns read-only page bytes.
    virtual ConstPageView Page() const = 0;

    /// Marks the page dirty with the latest WAL sequence number.
    virtual Status MarkDirty(LogSequenceNumber lsn) = 0;
};

class BufferPool {
public:
    /// Destroys a buffer pool through the interface pointer.
    virtual ~BufferPool() = default;

    /// Allocates and pins a new page.
    virtual Result<std::unique_ptr<PageHandle>> NewPage(PageKind kind) = 0;

    /// Pins an existing page for read or write access.
    virtual Result<std::unique_ptr<PageHandle>> PinPage(PageId page_id,
                                                        PageAccessMode mode) = 0;

    /// Flushes one dirty page if it is resident.
    virtual Status FlushPage(PageId page_id) = 0;

    /// Flushes all dirty pages.
    virtual Status FlushAll() = 0;
};

} // namespace mattsql
