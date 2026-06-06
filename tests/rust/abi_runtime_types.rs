#![allow(dead_code)]

use core::ffi::{c_char, c_void};

pub const MATTSQL_ABI_RUNTIME_VERSION: u32 = 1;

pub type MattsqlAbiStatusCode = u32;

pub const MATTSQL_ABI_STATUS_OK: MattsqlAbiStatusCode = 0;
pub const MATTSQL_ABI_STATUS_INVALID_ARGUMENT: MattsqlAbiStatusCode = 1;
pub const MATTSQL_ABI_STATUS_NOT_FOUND: MattsqlAbiStatusCode = 2;
pub const MATTSQL_ABI_STATUS_ALREADY_EXISTS: MattsqlAbiStatusCode = 3;
pub const MATTSQL_ABI_STATUS_TYPE_MISMATCH: MattsqlAbiStatusCode = 4;
pub const MATTSQL_ABI_STATUS_PARSE_ERROR: MattsqlAbiStatusCode = 5;
pub const MATTSQL_ABI_STATUS_BIND_ERROR: MattsqlAbiStatusCode = 6;
pub const MATTSQL_ABI_STATUS_PLAN_ERROR: MattsqlAbiStatusCode = 7;
pub const MATTSQL_ABI_STATUS_EXECUTION_ERROR: MattsqlAbiStatusCode = 8;
pub const MATTSQL_ABI_STATUS_IO_ERROR: MattsqlAbiStatusCode = 9;
pub const MATTSQL_ABI_STATUS_CORRUPTION: MattsqlAbiStatusCode = 10;
pub const MATTSQL_ABI_STATUS_TRANSACTION_CONFLICT: MattsqlAbiStatusCode = 11;
pub const MATTSQL_ABI_STATUS_NOT_SUPPORTED: MattsqlAbiStatusCode = 12;
pub const MATTSQL_ABI_STATUS_INTERNAL: MattsqlAbiStatusCode = 13;

pub type MattsqlAbiLogLevel = u32;

pub const MATTSQL_ABI_LOG_TRACE: MattsqlAbiLogLevel = 0;
pub const MATTSQL_ABI_LOG_DEBUG: MattsqlAbiLogLevel = 1;
pub const MATTSQL_ABI_LOG_INFO: MattsqlAbiLogLevel = 2;
pub const MATTSQL_ABI_LOG_WARN: MattsqlAbiLogLevel = 3;
pub const MATTSQL_ABI_LOG_ERROR: MattsqlAbiLogLevel = 4;
pub const MATTSQL_ABI_LOG_FATAL: MattsqlAbiLogLevel = 5;

pub type MattsqlAbiIoOperation = u32;

pub const MATTSQL_ABI_IO_READ: MattsqlAbiIoOperation = 0;
pub const MATTSQL_ABI_IO_WRITE: MattsqlAbiIoOperation = 1;
pub const MATTSQL_ABI_IO_FLUSH: MattsqlAbiIoOperation = 2;

pub type MattsqlAbiRuntimeMemoryFlags = u32;

pub const MATTSQL_ABI_MEMORY_NORMAL: MattsqlAbiRuntimeMemoryFlags = 0;
pub const MATTSQL_ABI_MEMORY_ZEROED: MattsqlAbiRuntimeMemoryFlags = 1 << 0;
pub const MATTSQL_ABI_MEMORY_DMA: MattsqlAbiRuntimeMemoryFlags = 1 << 1;

pub type MattsqlAbiIoRequestFlags = u32;

pub const MATTSQL_ABI_IO_REQUEST_NO_FLAGS: MattsqlAbiIoRequestFlags = 0;
pub const MATTSQL_ABI_IO_REQUEST_BARRIER_BEFORE: MattsqlAbiIoRequestFlags = 1 << 0;
pub const MATTSQL_ABI_IO_REQUEST_BARRIER_AFTER: MattsqlAbiIoRequestFlags = 1 << 1;
pub const MATTSQL_ABI_IO_REQUEST_FORCE_UNIT_ACCESS: MattsqlAbiIoRequestFlags =
    1 << 2;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct MattsqlAbiStatus {
    pub code: MattsqlAbiStatusCode,
    pub reserved: u32,
    pub message: *const c_char,
    pub message_length: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct MattsqlAbiRuntimeCapabilities {
    pub page_size: u64,
    pub block_size: u64,
    pub max_io_request_size: u64,
    pub max_io_batch_size: u64,
    pub max_outstanding_io: u64,
    pub supports_async_io: u8,
    pub supports_flush: u8,
    pub supports_barriers: u8,
    pub supports_physical_addresses: u8,
    pub supports_dma_memory: u8,
    pub reserved: [u8; 3],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct MattsqlAbiPageAllocation {
    pub data: *mut c_void,
    pub page_count: u64,
    pub page_size: u64,
    pub alignment: u64,
    pub flags: MattsqlAbiRuntimeMemoryFlags,
    pub reserved: u32,
    pub physical_address: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct MattsqlAbiIoRequest {
    pub id: u64,
    pub operation: MattsqlAbiIoOperation,
    pub flags: MattsqlAbiIoRequestFlags,
    pub offset: u64,
    pub buffer: *mut c_void,
    pub buffer_length: u64,
    pub length: u64,
    pub user_data: usize,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct MattsqlAbiIoCompletion {
    pub id: u64,
    pub status: MattsqlAbiStatus,
    pub bytes_transferred: u64,
    pub user_data: usize,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct MattsqlAbiIoSubmissionResult {
    pub submitted_count: u64,
    pub first_request_id: u64,
}

pub type MattsqlAbiGetCapabilitiesFn = Option<
    unsafe extern "C" fn(
        context: *mut c_void,
        out_capabilities: *mut MattsqlAbiRuntimeCapabilities,
    ) -> MattsqlAbiStatus,
>;

pub type MattsqlAbiAllocatePagesFn = Option<
    unsafe extern "C" fn(
        context: *mut c_void,
        page_count: u64,
        alignment: u64,
        flags: MattsqlAbiRuntimeMemoryFlags,
        out_allocation: *mut MattsqlAbiPageAllocation,
    ) -> MattsqlAbiStatus,
>;

pub type MattsqlAbiFreePagesFn = Option<
    unsafe extern "C" fn(
        context: *mut c_void,
        allocation: *const MattsqlAbiPageAllocation,
    ) -> MattsqlAbiStatus,
>;

pub type MattsqlAbiSubmitIoBatchFn = Option<
    unsafe extern "C" fn(
        context: *mut c_void,
        requests: *const MattsqlAbiIoRequest,
        request_count: u64,
        out_submission: *mut MattsqlAbiIoSubmissionResult,
    ) -> MattsqlAbiStatus,
>;

pub type MattsqlAbiPollIoCompletionsFn = Option<
    unsafe extern "C" fn(
        context: *mut c_void,
        completions: *mut MattsqlAbiIoCompletion,
        max_completion_count: u64,
        out_completion_count: *mut u64,
    ) -> MattsqlAbiStatus,
>;

pub type MattsqlAbiYieldFn =
    Option<unsafe extern "C" fn(context: *mut c_void) -> MattsqlAbiStatus>;

pub type MattsqlAbiMonotonicNanosFn = Option<
    unsafe extern "C" fn(context: *mut c_void, out_nanos: *mut u64) -> MattsqlAbiStatus,
>;

pub type MattsqlAbiLogFn = Option<
    unsafe extern "C" fn(
        context: *mut c_void,
        level: MattsqlAbiLogLevel,
        message: *const c_char,
        message_length: u64,
    ),
>;

pub type MattsqlAbiPanicFn = Option<
    unsafe extern "C" fn(
        context: *mut c_void,
        message: *const c_char,
        message_length: u64,
    ),
>;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct MattsqlAbiRuntimeV1 {
    pub version: u32,
    pub reserved: u32,
    pub context: *mut c_void,
    pub get_capabilities: MattsqlAbiGetCapabilitiesFn,
    pub allocate_pages: MattsqlAbiAllocatePagesFn,
    pub free_pages: MattsqlAbiFreePagesFn,
    pub submit_io_batch: MattsqlAbiSubmitIoBatchFn,
    pub poll_io_completions: MattsqlAbiPollIoCompletionsFn,
    pub yield_fn: MattsqlAbiYieldFn,
    pub monotonic_nanos: MattsqlAbiMonotonicNanosFn,
    pub log: MattsqlAbiLogFn,
    pub panic: MattsqlAbiPanicFn,
}
