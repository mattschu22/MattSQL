#ifndef MATTSQL_ABI_RUNTIME_H
#define MATTSQL_ABI_RUNTIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MATTSQL_ABI_RUNTIME_VERSION 1u

typedef uint32_t mattsql_abi_status_code;

#define MATTSQL_ABI_STATUS_OK 0u
#define MATTSQL_ABI_STATUS_INVALID_ARGUMENT 1u
#define MATTSQL_ABI_STATUS_NOT_FOUND 2u
#define MATTSQL_ABI_STATUS_ALREADY_EXISTS 3u
#define MATTSQL_ABI_STATUS_TYPE_MISMATCH 4u
#define MATTSQL_ABI_STATUS_PARSE_ERROR 5u
#define MATTSQL_ABI_STATUS_BIND_ERROR 6u
#define MATTSQL_ABI_STATUS_PLAN_ERROR 7u
#define MATTSQL_ABI_STATUS_EXECUTION_ERROR 8u
#define MATTSQL_ABI_STATUS_IO_ERROR 9u
#define MATTSQL_ABI_STATUS_CORRUPTION 10u
#define MATTSQL_ABI_STATUS_TRANSACTION_CONFLICT 11u
#define MATTSQL_ABI_STATUS_NOT_SUPPORTED 12u
#define MATTSQL_ABI_STATUS_INTERNAL 13u

typedef struct mattsql_abi_status {
  mattsql_abi_status_code code;
  uint32_t reserved;
  const char *message;
  uint64_t message_length;
} mattsql_abi_status;

typedef uint32_t mattsql_abi_log_level;

#define MATTSQL_ABI_LOG_TRACE 0u
#define MATTSQL_ABI_LOG_DEBUG 1u
#define MATTSQL_ABI_LOG_INFO 2u
#define MATTSQL_ABI_LOG_WARN 3u
#define MATTSQL_ABI_LOG_ERROR 4u
#define MATTSQL_ABI_LOG_FATAL 5u

typedef uint32_t mattsql_abi_io_operation;

#define MATTSQL_ABI_IO_READ 0u
#define MATTSQL_ABI_IO_WRITE 1u
#define MATTSQL_ABI_IO_FLUSH 2u

typedef uint32_t mattsql_abi_runtime_memory_flags;

#define MATTSQL_ABI_MEMORY_NORMAL 0u
#define MATTSQL_ABI_MEMORY_ZEROED (1u << 0u)
#define MATTSQL_ABI_MEMORY_DMA (1u << 1u)

typedef uint32_t mattsql_abi_io_request_flags;

#define MATTSQL_ABI_IO_REQUEST_NO_FLAGS 0u
#define MATTSQL_ABI_IO_REQUEST_BARRIER_BEFORE (1u << 0u)
#define MATTSQL_ABI_IO_REQUEST_BARRIER_AFTER (1u << 1u)
#define MATTSQL_ABI_IO_REQUEST_FORCE_UNIT_ACCESS (1u << 2u)

typedef struct mattsql_abi_runtime_capabilities {
  uint64_t page_size;
  uint64_t block_size;
  uint64_t max_io_request_size;
  uint64_t max_io_batch_size;
  uint64_t max_outstanding_io;
  uint8_t supports_async_io;
  uint8_t supports_flush;
  uint8_t supports_barriers;
  uint8_t supports_physical_addresses;
  uint8_t supports_dma_memory;
  uint8_t reserved[3];
} mattsql_abi_runtime_capabilities;

typedef struct mattsql_abi_page_allocation {
  void *data;
  uint64_t page_count;
  uint64_t page_size;
  uint64_t alignment;
  mattsql_abi_runtime_memory_flags flags;
  uint32_t reserved;
  uint64_t physical_address;
} mattsql_abi_page_allocation;

typedef struct mattsql_abi_io_request {
  uint64_t id;
  mattsql_abi_io_operation operation;
  mattsql_abi_io_request_flags flags;
  uint64_t offset;
  void *buffer;
  uint64_t buffer_length;
  uint64_t length;
  uintptr_t user_data;
} mattsql_abi_io_request;

typedef struct mattsql_abi_io_completion {
  uint64_t id;
  mattsql_abi_status status;
  uint64_t bytes_transferred;
  uintptr_t user_data;
} mattsql_abi_io_completion;

typedef struct mattsql_abi_io_submission_result {
  uint64_t submitted_count;
  uint64_t first_request_id;
} mattsql_abi_io_submission_result;

typedef mattsql_abi_status (*mattsql_abi_get_capabilities_fn)(
    void *context, mattsql_abi_runtime_capabilities *out_capabilities);

typedef mattsql_abi_status (*mattsql_abi_allocate_pages_fn)(
    void *context, uint64_t page_count, uint64_t alignment,
    mattsql_abi_runtime_memory_flags flags,
    mattsql_abi_page_allocation *out_allocation);

typedef mattsql_abi_status (*mattsql_abi_free_pages_fn)(
    void *context, const mattsql_abi_page_allocation *allocation);

typedef mattsql_abi_status (*mattsql_abi_submit_io_batch_fn)(
    void *context, const mattsql_abi_io_request *requests, uint64_t request_count,
    mattsql_abi_io_submission_result *out_submission);

typedef mattsql_abi_status (*mattsql_abi_poll_io_completions_fn)(
    void *context, mattsql_abi_io_completion *completions,
    uint64_t max_completion_count, uint64_t *out_completion_count);

typedef mattsql_abi_status (*mattsql_abi_yield_fn)(void *context);

typedef mattsql_abi_status (*mattsql_abi_monotonic_nanos_fn)(
    void *context, uint64_t *out_nanos);

typedef void (*mattsql_abi_log_fn)(void *context, mattsql_abi_log_level level,
                                   const char *message,
                                   uint64_t message_length);

typedef void (*mattsql_abi_panic_fn)(void *context, const char *message,
                                     uint64_t message_length);

typedef struct mattsql_abi_runtime_v1 {
  uint32_t version;
  uint32_t reserved;
  void *context;
  mattsql_abi_get_capabilities_fn get_capabilities;
  mattsql_abi_allocate_pages_fn allocate_pages;
  mattsql_abi_free_pages_fn free_pages;
  mattsql_abi_submit_io_batch_fn submit_io_batch;
  mattsql_abi_poll_io_completions_fn poll_io_completions;
  mattsql_abi_yield_fn yield;
  mattsql_abi_monotonic_nanos_fn monotonic_nanos;
  mattsql_abi_log_fn log;
  mattsql_abi_panic_fn panic;
} mattsql_abi_runtime_v1;

#ifdef __cplusplus
}
#endif

#endif
