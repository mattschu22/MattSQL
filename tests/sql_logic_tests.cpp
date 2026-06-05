#include "sql_logic_support.hpp"
#include "test_framework.hpp"

#include <cstdint>
#include <filesystem>

#ifndef MATTSQL_TEST_SOURCE_DIR
#define MATTSQL_TEST_SOURCE_DIR "."
#endif

/// Verifies SQL logic scripts exercise the hosted engine end-to-end.
TEST_CASE(sql_logic_integration_scripts_pass) {
  sql_logic::RunLogicDirectory(std::filesystem::path(MATTSQL_TEST_SOURCE_DIR) /
                               "tests" / "sqllogic");
}

/// Verifies random-but-reproducible SQL programs match a simple in-memory model.
TEST_CASE(sql_logic_deterministic_fuzzer_matches_model) {
  sql_logic::RunDeterministicFuzzerSeeds(std::uint64_t{0x5EED0000}, 8);
}
