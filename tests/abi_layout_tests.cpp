#include "mattsql/abi/runtime.h"

#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>

/// Verifies the version-1 status struct layout used across the C ABI.
TEST_CASE(c_abi_status_layout_is_stable) {
  EXPECT_EQ(sizeof(void *), 8U);
  EXPECT_EQ(sizeof(mattsql_abi_status), 24U);
  EXPECT_EQ(alignof(mattsql_abi_status), 8U);
  EXPECT_EQ(offsetof(mattsql_abi_status, code), 0U);
  EXPECT_EQ(offsetof(mattsql_abi_status, reserved), 4U);
  EXPECT_EQ(offsetof(mattsql_abi_status, message), 8U);
  EXPECT_EQ(offsetof(mattsql_abi_status, message_length), 16U);
}

/// Verifies the version-1 capabilities struct layout used across the C ABI.
TEST_CASE(c_abi_runtime_capabilities_layout_is_stable) {
  EXPECT_EQ(sizeof(mattsql_abi_runtime_capabilities), 48U);
  EXPECT_EQ(alignof(mattsql_abi_runtime_capabilities), 8U);
  EXPECT_EQ(offsetof(mattsql_abi_runtime_capabilities, page_size), 0U);
  EXPECT_EQ(offsetof(mattsql_abi_runtime_capabilities, block_size), 8U);
  EXPECT_EQ(offsetof(mattsql_abi_runtime_capabilities, max_io_request_size), 16U);
  EXPECT_EQ(offsetof(mattsql_abi_runtime_capabilities, max_io_batch_size), 24U);
  EXPECT_EQ(offsetof(mattsql_abi_runtime_capabilities, max_outstanding_io), 32U);
  EXPECT_EQ(offsetof(mattsql_abi_runtime_capabilities, supports_async_io), 40U);
  EXPECT_EQ(offsetof(mattsql_abi_runtime_capabilities, supports_flush), 41U);
  EXPECT_EQ(offsetof(mattsql_abi_runtime_capabilities, supports_barriers), 42U);
  EXPECT_EQ(offsetof(mattsql_abi_runtime_capabilities,
                     supports_physical_addresses),
            43U);
  EXPECT_EQ(offsetof(mattsql_abi_runtime_capabilities, supports_dma_memory), 44U);
  EXPECT_EQ(offsetof(mattsql_abi_runtime_capabilities, reserved), 45U);
}

/// Verifies the version-1 page allocation struct layout used across the C ABI.
TEST_CASE(c_abi_page_allocation_layout_is_stable) {
  EXPECT_EQ(sizeof(mattsql_abi_page_allocation), 48U);
  EXPECT_EQ(alignof(mattsql_abi_page_allocation), 8U);
  EXPECT_EQ(offsetof(mattsql_abi_page_allocation, data), 0U);
  EXPECT_EQ(offsetof(mattsql_abi_page_allocation, page_count), 8U);
  EXPECT_EQ(offsetof(mattsql_abi_page_allocation, page_size), 16U);
  EXPECT_EQ(offsetof(mattsql_abi_page_allocation, alignment), 24U);
  EXPECT_EQ(offsetof(mattsql_abi_page_allocation, flags), 32U);
  EXPECT_EQ(offsetof(mattsql_abi_page_allocation, reserved), 36U);
  EXPECT_EQ(offsetof(mattsql_abi_page_allocation, physical_address), 40U);
}

/// Verifies the version-1 I/O request struct layout used across the C ABI.
TEST_CASE(c_abi_io_request_layout_is_stable) {
  EXPECT_EQ(sizeof(mattsql_abi_io_request), 56U);
  EXPECT_EQ(alignof(mattsql_abi_io_request), 8U);
  EXPECT_EQ(offsetof(mattsql_abi_io_request, id), 0U);
  EXPECT_EQ(offsetof(mattsql_abi_io_request, operation), 8U);
  EXPECT_EQ(offsetof(mattsql_abi_io_request, flags), 12U);
  EXPECT_EQ(offsetof(mattsql_abi_io_request, offset), 16U);
  EXPECT_EQ(offsetof(mattsql_abi_io_request, buffer), 24U);
  EXPECT_EQ(offsetof(mattsql_abi_io_request, buffer_length), 32U);
  EXPECT_EQ(offsetof(mattsql_abi_io_request, length), 40U);
  EXPECT_EQ(offsetof(mattsql_abi_io_request, user_data), 48U);
}

/// Verifies the version-1 I/O completion struct layout used across the C ABI.
TEST_CASE(c_abi_io_completion_layout_is_stable) {
  EXPECT_EQ(sizeof(mattsql_abi_io_completion), 48U);
  EXPECT_EQ(alignof(mattsql_abi_io_completion), 8U);
  EXPECT_EQ(offsetof(mattsql_abi_io_completion, id), 0U);
  EXPECT_EQ(offsetof(mattsql_abi_io_completion, status), 8U);
  EXPECT_EQ(offsetof(mattsql_abi_io_completion, bytes_transferred), 32U);
  EXPECT_EQ(offsetof(mattsql_abi_io_completion, user_data), 40U);
}

/// Verifies the version-1 I/O submission result layout used across the C ABI.
TEST_CASE(c_abi_io_submission_result_layout_is_stable) {
  EXPECT_EQ(sizeof(mattsql_abi_io_submission_result), 16U);
  EXPECT_EQ(alignof(mattsql_abi_io_submission_result), 8U);
  EXPECT_EQ(offsetof(mattsql_abi_io_submission_result, submitted_count), 0U);
  EXPECT_EQ(offsetof(mattsql_abi_io_submission_result, first_request_id), 8U);
}

/// Verifies the version-1 runtime function table layout used across the C ABI.
TEST_CASE(c_abi_runtime_v1_layout_is_stable) {
  EXPECT_EQ(MATTSQL_ABI_RUNTIME_VERSION, 1U);
  EXPECT_EQ(sizeof(mattsql_abi_runtime_v1), 88U);
  EXPECT_EQ(alignof(mattsql_abi_runtime_v1), 8U);
  EXPECT_EQ(offsetof(mattsql_abi_runtime_v1, version), 0U);
  EXPECT_EQ(offsetof(mattsql_abi_runtime_v1, reserved), 4U);
  EXPECT_EQ(offsetof(mattsql_abi_runtime_v1, context), 8U);
  EXPECT_EQ(offsetof(mattsql_abi_runtime_v1, get_capabilities), 16U);
  EXPECT_EQ(offsetof(mattsql_abi_runtime_v1, allocate_pages), 24U);
  EXPECT_EQ(offsetof(mattsql_abi_runtime_v1, free_pages), 32U);
  EXPECT_EQ(offsetof(mattsql_abi_runtime_v1, submit_io_batch), 40U);
  EXPECT_EQ(offsetof(mattsql_abi_runtime_v1, poll_io_completions), 48U);
  EXPECT_EQ(offsetof(mattsql_abi_runtime_v1, yield), 56U);
  EXPECT_EQ(offsetof(mattsql_abi_runtime_v1, monotonic_nanos), 64U);
  EXPECT_EQ(offsetof(mattsql_abi_runtime_v1, log), 72U);
  EXPECT_EQ(offsetof(mattsql_abi_runtime_v1, panic), 80U);
}

