#pragma once

#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace test {

class Registry {
public:
  using TestBody = std::function<void()>;

  /// Returns the process-wide registry for all statically registered tests.
  static Registry& instance() {
    static Registry registry;
    return registry;
  }

  /// Adds a named test body to the registry.
  void add(std::string name, TestBody body) {
    tests_.push_back({std::move(name), std::move(body)});
  }

  /// Runs every registered test and returns a process exit code.
  int run() const {
    int failed = 0;

    for (const auto& current : tests_) {
      try {
        current.body();
        std::cout << "[PASS] " << current.name << '\n';
      } catch (const std::exception& error) {
        ++failed;
        std::cerr << "[FAIL] " << current.name << ": " << error.what()
                  << '\n';
      }
    }

    std::cout << tests_.size() - static_cast<std::size_t>(failed) << '/'
              << tests_.size() << " tests passed\n";
    return failed == 0 ? 0 : 1;
  }

private:
  struct TestCase {
    std::string name;
    TestBody body;
  };

  std::vector<TestCase> tests_;
};

class Registrar {
public:
  /// Registers a test body during static initialization.
  Registrar(std::string name, Registry::TestBody body) {
    Registry::instance().add(std::move(name), std::move(body));
  }
};

/// Throws a formatted assertion failure at the given source location.
inline void fail(std::string_view file, int line, std::string_view expression) {
  std::ostringstream message;
  message << file << ':' << line << ": expectation failed: " << expression;
  throw std::runtime_error(message.str());
}

/// Verifies that the supplied callable throws any exception type.
template <typename Callable>
void expect_throws(Callable&& callable, std::string_view file, int line,
                   std::string_view expression) {
  try {
    std::forward<Callable>(callable)();
  } catch (...) {
    return;
  }

  fail(file, line, expression);
}

} // namespace test

/// Defines and registers a named test case.
#define TEST_CASE(name)                                                        \
  static void name();                                                          \
  static const ::test::Registrar name##_registrar(#name, &name);               \
  static void name()

/// Fails the current test unless the expression evaluates to true.
#define EXPECT_TRUE(expression)                                                \
  do {                                                                         \
    if (!(expression)) {                                                       \
      ::test::fail(__FILE__, __LINE__, #expression);                           \
    }                                                                          \
  } while (false)

/// Fails the current test unless the actual and expected values compare equal.
#define EXPECT_EQ(actual, expected)                                            \
  do {                                                                         \
    const auto actual_value = (actual);                                        \
    const auto expected_value = (expected);                                    \
    if (!(actual_value == expected_value)) {                                   \
      std::ostringstream message;                                              \
      message << __FILE__ << ':' << __LINE__ << ": expected " << #actual       \
              << " == " << #expected << ", got `" << actual_value << "` and `" \
              << expected_value << '`';                                        \
      throw std::runtime_error(message.str());                                 \
    }                                                                          \
  } while (false)

/// Fails the current test unless the expression throws.
#define EXPECT_THROWS(expression)                                              \
  ::test::expect_throws([&] { expression; }, __FILE__, __LINE__, #expression)
