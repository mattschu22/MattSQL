#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace mattsql {

using TableId = std::uint64_t;
using ColumnId = std::uint32_t;
using IndexId = std::uint64_t;
using PageId = std::uint64_t;
using FrameId = std::uint32_t;
using TransactionId = std::uint64_t;
using LogSequenceNumber = std::uint64_t;
using StorageOffset = std::uint64_t;
using RuntimeTaskId = std::uint64_t;
using IoRequestId = std::uint64_t;

inline constexpr std::size_t kDefaultPageSize = 4096;
inline constexpr PageId kInvalidPageId = UINT64_MAX;
inline constexpr LogSequenceNumber kInvalidLsn = UINT64_MAX;

enum class SqlType {
    Integer,
    Text,
    Boolean,
    Null
};

struct NullValue {};

// Scalar boundary value for expression constants/results. Row and storage paths
// use encoded tuple/vector representations below instead of per-cell variants.
using Value = std::variant<NullValue, std::int64_t, bool, std::string>;

struct BufferView {
    std::span<std::byte> bytes;
};

struct ConstBufferView {
    std::span<const std::byte> bytes;
};

struct TupleView {
    // Encoded according to the table schema; no per-column objects are stored.
    ConstBufferView bytes;
};

struct MutableTupleView {
    BufferView bytes;
};

struct Tuple {
    // Owned encoded tuple payload used at materialization or storage boundaries.
    std::vector<std::byte> bytes;
};

struct TupleBatch {
    // Packed tuple payloads. offsets has row_count + 1 entries when populated.
    std::vector<std::byte> data;
    std::vector<std::uint32_t> offsets;
};

struct NullBitmap {
    // One bit per row/value; the exact bit numbering is part of the encoder.
    std::vector<std::uint64_t> words;
};

struct ColumnVector {
    // Columnar execution batch storage. Fixed-width types use fixed_width_data;
    // variable-width types use offsets into variable_width_data.
    SqlType type = SqlType::Null;
    std::size_t value_count = 0;
    NullBitmap nulls;
    std::vector<std::byte> fixed_width_data;
    std::vector<std::uint32_t> variable_offsets;
    std::vector<std::byte> variable_width_data;
};

struct VectorBatch {
    // Vectorized executor result. All columns must have row_count values.
    std::size_t row_count = 0;
    std::vector<ColumnVector> columns;
};

} // namespace mattsql
