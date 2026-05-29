#include "mattsql/mattsql.hpp"

#include "test_framework.hpp"

#include <type_traits>
#include <variant>

/// Verifies the aggregate interface header is self-contained.
TEST_CASE(sql_engine_interfaces_are_self_contained) {
  static_assert(std::is_enum_v<mattsql::ErrorCode>);
  static_assert(std::is_enum_v<mattsql::LogicalOperatorKind>);
  static_assert(std::is_enum_v<mattsql::PhysicalOperatorKind>);
  static_assert(std::is_enum_v<mattsql::TransactionState>);
  static_assert(std::variant_size_v<mattsql::Value> == 4);
  static_assert(std::is_default_constructible_v<mattsql::TupleBatch>);
  static_assert(std::is_default_constructible_v<mattsql::VectorBatch>);
  EXPECT_TRUE(true);
}
