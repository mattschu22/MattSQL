#![allow(dead_code)]
#![no_std]

use core::ffi::{c_char, c_void};
use core::mem::{align_of, size_of};

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

const _: [(); 1] = [(); MATTSQL_ABI_RUNTIME_VERSION as usize];

const _: [(); 24] = [(); size_of::<MattsqlAbiStatus>()];
const _: [(); 8] = [(); align_of::<MattsqlAbiStatus>()];
const _: [(); 0] = [(); core::mem::offset_of!(MattsqlAbiStatus, code)];
const _: [(); 4] = [(); core::mem::offset_of!(MattsqlAbiStatus, reserved)];
const _: [(); 8] = [(); core::mem::offset_of!(MattsqlAbiStatus, message)];
const _: [(); 16] = [(); core::mem::offset_of!(MattsqlAbiStatus, message_length)];

const _: [(); 48] = [(); size_of::<MattsqlAbiRuntimeCapabilities>()];
const _: [(); 8] = [(); align_of::<MattsqlAbiRuntimeCapabilities>()];
const _: [(); 0] = [(); core::mem::offset_of!(MattsqlAbiRuntimeCapabilities, page_size)];
const _: [(); 8] = [(); core::mem::offset_of!(MattsqlAbiRuntimeCapabilities, block_size)];
const _: [(); 16] =
    [(); core::mem::offset_of!(MattsqlAbiRuntimeCapabilities, max_io_request_size)];
const _: [(); 24] =
    [(); core::mem::offset_of!(MattsqlAbiRuntimeCapabilities, max_io_batch_size)];
const _: [(); 32] =
    [(); core::mem::offset_of!(MattsqlAbiRuntimeCapabilities, max_outstanding_io)];
const _: [(); 40] =
    [(); core::mem::offset_of!(MattsqlAbiRuntimeCapabilities, supports_async_io)];
const _: [(); 41] =
    [(); core::mem::offset_of!(MattsqlAbiRuntimeCapabilities, supports_flush)];
const _: [(); 42] =
    [(); core::mem::offset_of!(MattsqlAbiRuntimeCapabilities, supports_barriers)];
const _: [(); 43] =
    [(); core::mem::offset_of!(MattsqlAbiRuntimeCapabilities, supports_physical_addresses)];
const _: [(); 44] =
    [(); core::mem::offset_of!(MattsqlAbiRuntimeCapabilities, supports_dma_memory)];
const _: [(); 45] = [(); core::mem::offset_of!(MattsqlAbiRuntimeCapabilities, reserved)];

const _: [(); 48] = [(); size_of::<MattsqlAbiPageAllocation>()];
const _: [(); 8] = [(); align_of::<MattsqlAbiPageAllocation>()];
const _: [(); 0] = [(); core::mem::offset_of!(MattsqlAbiPageAllocation, data)];
const _: [(); 8] = [(); core::mem::offset_of!(MattsqlAbiPageAllocation, page_count)];
const _: [(); 16] = [(); core::mem::offset_of!(MattsqlAbiPageAllocation, page_size)];
const _: [(); 24] = [(); core::mem::offset_of!(MattsqlAbiPageAllocation, alignment)];
const _: [(); 32] = [(); core::mem::offset_of!(MattsqlAbiPageAllocation, flags)];
const _: [(); 36] = [(); core::mem::offset_of!(MattsqlAbiPageAllocation, reserved)];
const _: [(); 40] =
    [(); core::mem::offset_of!(MattsqlAbiPageAllocation, physical_address)];

const _: [(); 56] = [(); size_of::<MattsqlAbiIoRequest>()];
const _: [(); 8] = [(); align_of::<MattsqlAbiIoRequest>()];
const _: [(); 0] = [(); core::mem::offset_of!(MattsqlAbiIoRequest, id)];
const _: [(); 8] = [(); core::mem::offset_of!(MattsqlAbiIoRequest, operation)];
const _: [(); 12] = [(); core::mem::offset_of!(MattsqlAbiIoRequest, flags)];
const _: [(); 16] = [(); core::mem::offset_of!(MattsqlAbiIoRequest, offset)];
const _: [(); 24] = [(); core::mem::offset_of!(MattsqlAbiIoRequest, buffer)];
const _: [(); 32] = [(); core::mem::offset_of!(MattsqlAbiIoRequest, buffer_length)];
const _: [(); 40] = [(); core::mem::offset_of!(MattsqlAbiIoRequest, length)];
const _: [(); 48] = [(); core::mem::offset_of!(MattsqlAbiIoRequest, user_data)];

const _: [(); 48] = [(); size_of::<MattsqlAbiIoCompletion>()];
const _: [(); 8] = [(); align_of::<MattsqlAbiIoCompletion>()];
const _: [(); 0] = [(); core::mem::offset_of!(MattsqlAbiIoCompletion, id)];
const _: [(); 8] = [(); core::mem::offset_of!(MattsqlAbiIoCompletion, status)];
const _: [(); 32] =
    [(); core::mem::offset_of!(MattsqlAbiIoCompletion, bytes_transferred)];
const _: [(); 40] = [(); core::mem::offset_of!(MattsqlAbiIoCompletion, user_data)];

const _: [(); 16] = [(); size_of::<MattsqlAbiIoSubmissionResult>()];
const _: [(); 8] = [(); align_of::<MattsqlAbiIoSubmissionResult>()];
const _: [(); 0] =
    [(); core::mem::offset_of!(MattsqlAbiIoSubmissionResult, submitted_count)];
const _: [(); 8] =
    [(); core::mem::offset_of!(MattsqlAbiIoSubmissionResult, first_request_id)];

const _: [(); 88] = [(); size_of::<MattsqlAbiRuntimeV1>()];
const _: [(); 8] = [(); align_of::<MattsqlAbiRuntimeV1>()];
const _: [(); 0] = [(); core::mem::offset_of!(MattsqlAbiRuntimeV1, version)];
const _: [(); 4] = [(); core::mem::offset_of!(MattsqlAbiRuntimeV1, reserved)];
const _: [(); 8] = [(); core::mem::offset_of!(MattsqlAbiRuntimeV1, context)];
const _: [(); 16] = [(); core::mem::offset_of!(MattsqlAbiRuntimeV1, get_capabilities)];
const _: [(); 24] = [(); core::mem::offset_of!(MattsqlAbiRuntimeV1, allocate_pages)];
const _: [(); 32] = [(); core::mem::offset_of!(MattsqlAbiRuntimeV1, free_pages)];
const _: [(); 40] = [(); core::mem::offset_of!(MattsqlAbiRuntimeV1, submit_io_batch)];
const _: [(); 48] =
    [(); core::mem::offset_of!(MattsqlAbiRuntimeV1, poll_io_completions)];
const _: [(); 56] = [(); core::mem::offset_of!(MattsqlAbiRuntimeV1, yield_fn)];
const _: [(); 64] = [(); core::mem::offset_of!(MattsqlAbiRuntimeV1, monotonic_nanos)];
const _: [(); 72] = [(); core::mem::offset_of!(MattsqlAbiRuntimeV1, log)];
const _: [(); 80] = [(); core::mem::offset_of!(MattsqlAbiRuntimeV1, panic)];
