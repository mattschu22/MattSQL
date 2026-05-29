#pragma once

#include "mattsql/common/types.hpp"

#include <cstddef>
#include <cstdint>

namespace mattsql {

enum class PageKind : std::uint16_t {
    Free,
    Heap,
    BTreeInternal,
    BTreeLeaf,
    Catalog,
    Wal
};

struct PageHeader {
    PageId page_id = kInvalidPageId;
    PageKind kind = PageKind::Free;
    LogSequenceNumber page_lsn = kInvalidLsn;
    std::uint16_t flags = 0;
    std::uint16_t lower = 0;
    std::uint16_t upper = 0;
};

struct PageView {
    PageHeader* header = nullptr;
    BufferView bytes;
};

struct ConstPageView {
    const PageHeader* header = nullptr;
    ConstBufferView bytes;
};

} // namespace mattsql
