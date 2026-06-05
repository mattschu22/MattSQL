#pragma once

#include "mattsql/common/types.hpp"
#include "test_framework.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace test {

inline mattsql::ConstBufferView const_view(const std::vector<std::byte> &bytes) {
  return mattsql::ConstBufferView{
      std::span<const std::byte>(bytes.data(), bytes.size())};
}

inline mattsql::BufferView mutable_view(std::vector<std::byte> &bytes) {
  return mattsql::BufferView{std::span<std::byte>(bytes.data(), bytes.size())};
}

inline void expect_bytes_equal(const std::vector<std::byte> &actual,
                               const std::vector<std::byte> &expected) {
  EXPECT_EQ(actual.size(), expected.size());
  for (std::size_t index = 0; index < actual.size(); ++index) {
    EXPECT_TRUE(actual[index] == expected[index]);
  }
}

} // namespace test
