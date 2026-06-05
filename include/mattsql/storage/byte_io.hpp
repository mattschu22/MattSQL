#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

namespace mattsql {

inline void AppendByte(std::vector<std::byte> &bytes, std::uint8_t value) {
  bytes.push_back(static_cast<std::byte>(value));
}

template <typename UInt>
void AppendLittleEndian(std::vector<std::byte> &bytes, UInt value) {
  static_assert(std::is_unsigned_v<UInt>);
  for (int shift = 0; shift < static_cast<int>(sizeof(UInt) * 8U); shift += 8) {
    AppendByte(bytes, static_cast<std::uint8_t>((value >> shift) & UInt{0xff}));
  }
}

template <typename UInt>
[[nodiscard]] bool ReadLittleEndian(std::span<const std::byte> bytes,
                                    std::size_t &offset, UInt &value) {
  static_assert(std::is_unsigned_v<UInt>);
  if (offset > bytes.size() || bytes.size() - offset < sizeof(UInt)) {
    return false;
  }

  value = 0;
  for (int shift = 0; shift < static_cast<int>(sizeof(UInt) * 8U); shift += 8) {
    value |= static_cast<UInt>(std::to_integer<std::uint8_t>(bytes[offset])) << shift;
    ++offset;
  }
  return true;
}

template <typename UInt>
[[nodiscard]] UInt ReadLittleEndianAt(std::span<const std::byte> bytes,
                                      std::size_t offset) {
  UInt value = 0;
  (void)ReadLittleEndian(bytes, offset, value);
  return value;
}

template <typename UInt>
void WriteLittleEndianAt(std::span<std::byte> bytes, std::size_t offset, UInt value) {
  static_assert(std::is_unsigned_v<UInt>);
  for (int shift = 0; shift < static_cast<int>(sizeof(UInt) * 8U); shift += 8) {
    bytes[offset] = static_cast<std::byte>((value >> shift) & static_cast<UInt>(0xff));
    ++offset;
  }
}

} // namespace mattsql
