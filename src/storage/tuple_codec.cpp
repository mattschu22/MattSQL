#include "mattsql/storage/tuple_codec.hpp"

#include "mattsql/common/result_utils.hpp"
#include "mattsql/common/trace.hpp"
#include "mattsql/common/value_utils.hpp"
#include "mattsql/storage/byte_io.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace mattsql {
namespace {

enum class FieldTag : std::uint8_t { Null = 0, Integer = 1, Boolean = 2, Text = 3 };

void append_u32(std::vector<std::byte> &bytes, std::uint32_t value) {
  AppendLittleEndian(bytes, value);
}

void append_i64(std::vector<std::byte> &bytes, std::int64_t value) {
  AppendLittleEndian(bytes, static_cast<std::uint64_t>(value));
}

[[nodiscard]] bool read_byte(ConstBufferView record, std::size_t &offset,
                             std::uint8_t &value) {
  if (offset >= record.bytes.size()) {
    return false;
  }

  value = static_cast<std::uint8_t>(record.bytes[offset]);
  ++offset;
  return true;
}

[[nodiscard]] bool read_u32(ConstBufferView record, std::size_t &offset,
                            std::uint32_t &value) {
  return ReadLittleEndian(record.bytes, offset, value);
}

[[nodiscard]] bool read_i64(ConstBufferView record, std::size_t &offset,
                            std::int64_t &value) {
  std::uint64_t encoded = 0;
  if (!ReadLittleEndian(record.bytes, offset, encoded)) {
    return false;
  }

  value = static_cast<std::int64_t>(encoded);
  return true;
}

} // namespace

Result<Tuple> BinaryTupleCodec::Encode(const TableSchema &schema,
                                       const std::vector<Value> &values) const {
  ScopedTrace trace("mattsql::BinaryTupleCodec::Encode", "function.storage");
  if (values.size() != schema.columns.size()) {
    return error_result<Tuple>(ErrorCode::InvalidArgument,
                               "tuple value count does not match schema");
  }

  Tuple tuple;
  for (std::size_t index = 0; index < schema.columns.size(); ++index) {
    const auto &column = schema.columns[index];
    const auto &value = values[index];

    if (column.type == SqlType::Null) {
      return error_result<Tuple>(ErrorCode::InvalidArgument,
                                 "storage column type cannot be Null");
    }

    if (std::holds_alternative<NullValue>(value)) {
      if (!column.nullable) {
        return error_result<Tuple>(ErrorCode::TypeMismatch,
                                   "column cannot be NULL: " + column.name);
      }
      AppendByte(tuple.bytes, static_cast<std::uint8_t>(FieldTag::Null));
      continue;
    }

    if (!ValueMatchesType(value, column.type)) {
      return error_result<Tuple>(ErrorCode::TypeMismatch,
                                 "value type does not match column: " + column.name);
    }

    switch (column.type) {
    case SqlType::Integer:
      AppendByte(tuple.bytes, static_cast<std::uint8_t>(FieldTag::Integer));
      append_i64(tuple.bytes, std::get<std::int64_t>(value));
      break;
    case SqlType::Text: {
      const auto &text = std::get<std::string>(value);
      if (text.size() > std::numeric_limits<std::uint32_t>::max()) {
        return error_result<Tuple>(ErrorCode::InvalidArgument,
                                   "text value is too large");
      }
      AppendByte(tuple.bytes, static_cast<std::uint8_t>(FieldTag::Text));
      append_u32(tuple.bytes, static_cast<std::uint32_t>(text.size()));
      for (const char character : text) {
        AppendByte(tuple.bytes, static_cast<std::uint8_t>(character));
      }
      break;
    }
    case SqlType::Boolean:
      AppendByte(tuple.bytes, static_cast<std::uint8_t>(FieldTag::Boolean));
      AppendByte(tuple.bytes, std::get<bool>(value) ? 1U : 0U);
      break;
    case SqlType::Null:
      break;
    }
  }

  return ok_result(std::move(tuple));
}

Result<std::vector<Value>> BinaryTupleCodec::Decode(const TableSchema &schema,
                                                    ConstBufferView record) const {
  ScopedTrace trace("mattsql::BinaryTupleCodec::Decode", "function.storage");
  std::vector<Value> values;
  values.reserve(schema.columns.size());

  std::size_t offset = 0;
  for (const auto &column : schema.columns) {
    if (column.type == SqlType::Null) {
      return error_result<std::vector<Value>>(ErrorCode::Corruption,
                                              "storage column type cannot be Null");
    }

    std::uint8_t tag_byte = 0;
    if (!read_byte(record, offset, tag_byte)) {
      return error_result<std::vector<Value>>(ErrorCode::Corruption,
                                              "tuple record is truncated");
    }

    const auto tag = static_cast<FieldTag>(tag_byte);
    if (tag == FieldTag::Null) {
      if (!column.nullable) {
        return error_result<std::vector<Value>>(
            ErrorCode::Corruption, "non-nullable column stored NULL: " + column.name);
      }
      values.push_back(NullValue{});
      continue;
    }

    switch (column.type) {
    case SqlType::Integer: {
      if (tag != FieldTag::Integer) {
        return error_result<std::vector<Value>>(ErrorCode::Corruption,
                                                "tuple field type mismatch");
      }
      std::int64_t value = 0;
      if (!read_i64(record, offset, value)) {
        return error_result<std::vector<Value>>(ErrorCode::Corruption,
                                                "tuple integer is truncated");
      }
      values.push_back(value);
      break;
    }
    case SqlType::Text: {
      if (tag != FieldTag::Text) {
        return error_result<std::vector<Value>>(ErrorCode::Corruption,
                                                "tuple field type mismatch");
      }
      std::uint32_t length = 0;
      if (!read_u32(record, offset, length) || record.bytes.size() - offset < length) {
        return error_result<std::vector<Value>>(ErrorCode::Corruption,
                                                "tuple text is truncated");
      }
      std::string value;
      value.reserve(length);
      for (std::uint32_t index = 0; index < length; ++index) {
        value.push_back(static_cast<char>(record.bytes[offset + index]));
      }
      offset += length;
      values.push_back(std::move(value));
      break;
    }
    case SqlType::Boolean: {
      if (tag != FieldTag::Boolean) {
        return error_result<std::vector<Value>>(ErrorCode::Corruption,
                                                "tuple field type mismatch");
      }
      std::uint8_t value = 0;
      if (!read_byte(record, offset, value) || value > 1) {
        return error_result<std::vector<Value>>(ErrorCode::Corruption,
                                                "tuple boolean is invalid");
      }
      values.push_back(value == 1);
      break;
    }
    case SqlType::Null:
      break;
    }
  }

  if (offset != record.bytes.size()) {
    return error_result<std::vector<Value>>(ErrorCode::Corruption,
                                            "tuple record has trailing bytes");
  }

  return ok_result(std::move(values));
}

} // namespace mattsql
