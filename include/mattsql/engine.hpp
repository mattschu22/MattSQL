#pragma once

#include "mattsql/common/types.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mattsql {

struct QueryResult {
  std::vector<std::string> columns;
  std::vector<std::vector<Value>> rows;
};

class Engine {
public:
  Engine();
  ~Engine();

  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  Engine(Engine&&) noexcept;
  Engine& operator=(Engine&&) noexcept;

  /// Executes a single SQL statement and returns a tabular result.
  QueryResult execute(std::string_view sql);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Returns the MattSQL project version string.
std::string version();

} // namespace mattsql
