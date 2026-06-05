#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace mattsql {

[[nodiscard]] inline std::string FoldIdentifierKey(std::string_view value) {
  std::string key;
  key.reserve(value.size());

  for (const char character : value) {
    key.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
  }

  return key;
}

} // namespace mattsql
