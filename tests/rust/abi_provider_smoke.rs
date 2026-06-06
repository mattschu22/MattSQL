mod abi_runtime_types;

use abi_runtime_types::*;
use core::ffi::{c_char, c_int, c_void};
use core::ptr;

const PAGE_SIZE: u64 = 4096;
const BLOCK_SIZE: u64 = 32;
const STORAGE_SIZE: usize = 1024;
const MAX_IO_BATCH: u64 = 4;
const COMPLETION_CAPACITY: usize = 8;
const KNOWN_MEMORY_FLAGS: MattsqlAbiRuntimeMemoryFlags =
    MATTSQL_ABI_MEMORY_ZEROED | MATTSQL_ABI_MEMORY_DMA;
const KNOWN_IO_REQUEST_FLAGS: MattsqlAbiIoRequestFlags =
    MATTSQL_ABI_IO_REQUEST_BARRIER_BEFORE
        | MATTSQL_ABI_IO_REQUEST_BARRIER_AFTER
        | MATTSQL_ABI_IO_REQUEST_FORCE_UNIT_ACCESS;

extern "C" {
    fn posix_memalign(memptr: *mut *mut c_void, alignment: usize, size: usize) -> c_int;
    fn free(ptr: *mut c_void);
    fn memset(ptr: *mut c_void, value: c_int, size: usize) -> *mut c_void;
    fn memcpy(destination: *mut c_void, source: *const c_void, size: usize) -> *mut c_void;
}

const fn empty_status() -> MattsqlAbiStatus {
    MattsqlAbiStatus {
        code: MATTSQL_ABI_STATUS_OK,
        reserved: 0,
        message: ptr::null(),
        message_length: 0,
    }
}

const fn empty_completion() -> MattsqlAbiIoCompletion {
    MattsqlAbiIoCompletion {
        id: 0,
        status: empty_status(),
        bytes_transferred: 0,
        user_data: 0,
    }
}

struct SmokeRuntimeState {
    storage: [u8; STORAGE_SIZE],
    completions: [MattsqlAbiIoCompletion; COMPLETION_CAPACITY],
    completion_head: usize,
    completion_len: usize,
    next_request_id: u64,
    nanos: u64,
    yielded: u8,
    last_log_level: MattsqlAbiLogLevel,
}

impl SmokeRuntimeState {
    const fn new() -> Self {
        Self {
            storage: [0; STORAGE_SIZE],
            completions: [empty_completion(); COMPLETION_CAPACITY],
            completion_head: 0,
            completion_len: 0,
            next_request_id: 1,
            nanos: 1_000_000,
            yielded: 0,
            last_log_level: MATTSQL_ABI_LOG_TRACE,
        }
    }
}

static mut SMOKE_STATE: SmokeRuntimeState = SmokeRuntimeState::new();

fn ok_status() -> MattsqlAbiStatus {
    empty_status()
}

fn error_status(code: MattsqlAbiStatusCode, message: &'static [u8]) -> MattsqlAbiStatus {
    MattsqlAbiStatus {
        code,
        reserved: 0,
        message: message.as_ptr().cast::<c_char>(),
        message_length: message.len() as u64,
    }
}

fn invalid_argument(message: &'static [u8]) -> MattsqlAbiStatus {
    error_status(MATTSQL_ABI_STATUS_INVALID_ARGUMENT, message)
}

fn not_supported(message: &'static [u8]) -> MattsqlAbiStatus {
    error_status(MATTSQL_ABI_STATUS_NOT_SUPPORTED, message)
}

fn io_error(message: &'static [u8]) -> MattsqlAbiStatus {
    error_status(MATTSQL_ABI_STATUS_IO_ERROR, message)
}

fn state_ptr() -> *mut SmokeRuntimeState {
    ptr::addr_of_mut!(SMOKE_STATE)
}

unsafe fn state_from_context(context: *mut c_void) -> Option<&'static mut SmokeRuntimeState> {
    if context != state_ptr().cast::<c_void>() {
        return None;
    }
    Some(&mut *state_ptr())
}

fn is_power_of_two(value: u64) -> bool {
    value != 0 && (value & (value - 1)) == 0
}

fn effective_length(request: &MattsqlAbiIoRequest) -> u64 {
    if request.length != 0 {
        request.length
    } else {
        request.buffer_length
    }
}

fn validate_request(request: &MattsqlAbiIoRequest) -> MattsqlAbiStatus {
    if request.flags & !KNOWN_IO_REQUEST_FLAGS != 0 {
        return invalid_argument(b"unknown I/O request flags");
    }

    let length = effective_length(request);
    match request.operation {
        MATTSQL_ABI_IO_READ | MATTSQL_ABI_IO_WRITE => {
            if request.buffer.is_null() || request.buffer_length == 0 || length == 0 {
                return invalid_argument(b"buffered I/O request is empty");
            }
            if length > request.buffer_length {
                return invalid_argument(b"I/O request length exceeds its buffer");
            }
            if request.offset % BLOCK_SIZE != 0 || length % BLOCK_SIZE != 0 {
                return invalid_argument(b"I/O request is not block aligned");
            }
            let end = match request.offset.checked_add(length) {
                Some(value) => value,
                None => return invalid_argument(b"I/O request range overflows"),
            };
            if end > STORAGE_SIZE as u64 {
                return invalid_argument(b"I/O request exceeds smoke device");
            }
            ok_status()
        }
        MATTSQL_ABI_IO_FLUSH => {
            if !request.buffer.is_null() && request.buffer_length == 0 {
                return invalid_argument(b"I/O request has buffer without length");
            }
            ok_status()
        }
        _ => invalid_argument(b"unknown I/O operation"),
    }
}

fn push_completion(
    state: &mut SmokeRuntimeState,
    completion: MattsqlAbiIoCompletion,
) -> bool {
    if state.completion_len == COMPLETION_CAPACITY {
        return false;
    }
    let index = (state.completion_head + state.completion_len) % COMPLETION_CAPACITY;
    state.completions[index] = completion;
    state.completion_len += 1;
    true
}

unsafe extern "C" fn get_capabilities(
    context: *mut c_void,
    out_capabilities: *mut MattsqlAbiRuntimeCapabilities,
) -> MattsqlAbiStatus {
    if state_from_context(context).is_none() || out_capabilities.is_null() {
        return invalid_argument(b"missing capabilities argument");
    }

    *out_capabilities = MattsqlAbiRuntimeCapabilities {
        page_size: PAGE_SIZE,
        block_size: BLOCK_SIZE,
        max_io_request_size: STORAGE_SIZE as u64,
        max_io_batch_size: MAX_IO_BATCH,
        max_outstanding_io: COMPLETION_CAPACITY as u64,
        supports_async_io: 0,
        supports_flush: 1,
        supports_barriers: 1,
        supports_physical_addresses: 0,
        supports_dma_memory: 0,
        reserved: [0; 3],
    };
    ok_status()
}

unsafe extern "C" fn allocate_pages(
    context: *mut c_void,
    page_count: u64,
    alignment: u64,
    flags: MattsqlAbiRuntimeMemoryFlags,
    out_allocation: *mut MattsqlAbiPageAllocation,
) -> MattsqlAbiStatus {
    if state_from_context(context).is_none() || out_allocation.is_null() || page_count == 0 {
        return invalid_argument(b"invalid page allocation request");
    }
    if flags & !KNOWN_MEMORY_FLAGS != 0 {
        return invalid_argument(b"unknown page allocation flags");
    }
    if flags & MATTSQL_ABI_MEMORY_DMA != 0 {
        return not_supported(b"DMA memory is not supported");
    }

    let requested_alignment = if alignment == 0 { PAGE_SIZE } else { alignment };
    if !is_power_of_two(requested_alignment) {
        return invalid_argument(b"page allocation alignment is invalid");
    }
    let byte_count = match page_count.checked_mul(PAGE_SIZE) {
        Some(value) => value,
        None => return invalid_argument(b"page allocation is too large"),
    };
    let alignment_usize = match usize::try_from(requested_alignment) {
        Ok(value) => value,
        Err(_) => return invalid_argument(b"page allocation alignment is too large"),
    };
    let byte_count_usize = match usize::try_from(byte_count) {
        Ok(value) => value,
        Err(_) => return invalid_argument(b"page allocation is too large"),
    };

    let mut data: *mut c_void = ptr::null_mut();
    if posix_memalign(&mut data, alignment_usize, byte_count_usize) != 0 || data.is_null() {
        return error_status(MATTSQL_ABI_STATUS_INTERNAL, b"page allocation failed");
    }
    if flags & MATTSQL_ABI_MEMORY_ZEROED != 0 {
        memset(data, 0, byte_count_usize);
    }

    *out_allocation = MattsqlAbiPageAllocation {
        data,
        page_count,
        page_size: PAGE_SIZE,
        alignment: requested_alignment,
        flags,
        reserved: 0,
        physical_address: 0,
    };
    ok_status()
}

unsafe extern "C" fn free_pages(
    context: *mut c_void,
    allocation: *const MattsqlAbiPageAllocation,
) -> MattsqlAbiStatus {
    if state_from_context(context).is_none() || allocation.is_null() {
        return invalid_argument(b"invalid page allocation");
    }

    let allocation = &*allocation;
    if allocation.data.is_null()
        || allocation.page_count == 0
        || allocation.page_size != PAGE_SIZE
        || allocation.alignment == 0
        || !is_power_of_two(allocation.alignment)
        || allocation.flags & !KNOWN_MEMORY_FLAGS != 0
    {
        return invalid_argument(b"invalid page allocation metadata");
    }

    free(allocation.data);
    ok_status()
}

unsafe extern "C" fn submit_io_batch(
    context: *mut c_void,
    requests: *const MattsqlAbiIoRequest,
    request_count: u64,
    out_submission: *mut MattsqlAbiIoSubmissionResult,
) -> MattsqlAbiStatus {
    let state = match state_from_context(context) {
        Some(value) => value,
        None => return invalid_argument(b"invalid I/O batch"),
    };
    if requests.is_null() || out_submission.is_null() || request_count == 0 {
        return invalid_argument(b"invalid I/O batch");
    }
    if request_count > MAX_IO_BATCH {
        return invalid_argument(b"I/O batch is too large");
    }
    if request_count as usize > COMPLETION_CAPACITY - state.completion_len {
        return io_error(b"completion queue is full");
    }

    for index in 0..request_count {
        let request = &*requests.add(index as usize);
        let status = validate_request(request);
        if status.code != MATTSQL_ABI_STATUS_OK {
            return status;
        }
    }

    let mut first_request_id = 0;
    for index in 0..request_count {
        let request = &*requests.add(index as usize);
        let request_id = if request.id == 0 {
            let next = state.next_request_id;
            state.next_request_id += 1;
            next
        } else {
            request.id
        };
        if index == 0 {
            first_request_id = request_id;
        }

        let length = effective_length(request);
        if request.operation == MATTSQL_ABI_IO_WRITE {
            memcpy(
                state.storage.as_mut_ptr().add(request.offset as usize).cast::<c_void>(),
                request.buffer.cast_const(),
                length as usize,
            );
        } else if request.operation == MATTSQL_ABI_IO_READ {
            memcpy(
                request.buffer,
                state.storage.as_ptr().add(request.offset as usize).cast::<c_void>(),
                length as usize,
            );
        }

        let bytes_transferred = match request.operation {
            MATTSQL_ABI_IO_READ | MATTSQL_ABI_IO_WRITE => length,
            _ => 0,
        };
        let completion = MattsqlAbiIoCompletion {
            id: request_id,
            status: ok_status(),
            bytes_transferred,
            user_data: request.user_data,
        };
        if !push_completion(state, completion) {
            return io_error(b"completion queue is full");
        }
    }

    *out_submission = MattsqlAbiIoSubmissionResult {
        submitted_count: request_count,
        first_request_id,
    };
    ok_status()
}

unsafe extern "C" fn poll_io_completions(
    context: *mut c_void,
    completions: *mut MattsqlAbiIoCompletion,
    max_completion_count: u64,
    out_completion_count: *mut u64,
) -> MattsqlAbiStatus {
    let state = match state_from_context(context) {
        Some(value) => value,
        None => return invalid_argument(b"invalid completion poll"),
    };
    if completions.is_null() || out_completion_count.is_null() || max_completion_count == 0 {
        return invalid_argument(b"invalid completion poll");
    }

    let max_count = usize::try_from(max_completion_count)
        .unwrap_or(usize::MAX)
        .min(state.completion_len);
    for index in 0..max_count {
        let ring_index = (state.completion_head + index) % COMPLETION_CAPACITY;
        *completions.add(index) = state.completions[ring_index];
        state.completions[ring_index] = empty_completion();
    }
    state.completion_head = (state.completion_head + max_count) % COMPLETION_CAPACITY;
    state.completion_len -= max_count;
    *out_completion_count = max_count as u64;
    ok_status()
}

unsafe extern "C" fn yield_runtime(context: *mut c_void) -> MattsqlAbiStatus {
    let state = match state_from_context(context) {
        Some(value) => value,
        None => return invalid_argument(b"missing context"),
    };
    state.yielded = 1;
    ok_status()
}

unsafe extern "C" fn monotonic_nanos(
    context: *mut c_void,
    out_nanos: *mut u64,
) -> MattsqlAbiStatus {
    let state = match state_from_context(context) {
        Some(value) => value,
        None => return invalid_argument(b"missing clock output"),
    };
    if out_nanos.is_null() {
        return invalid_argument(b"missing clock output");
    }
    state.nanos += 1;
    *out_nanos = state.nanos;
    ok_status()
}

unsafe extern "C" fn log_message(
    context: *mut c_void,
    level: MattsqlAbiLogLevel,
    _message: *const c_char,
    _message_length: u64,
) {
    if let Some(state) = state_from_context(context) {
        state.last_log_level = level;
    }
}

unsafe extern "C" fn panic_runtime(
    _context: *mut c_void,
    _message: *const c_char,
    _message_length: u64,
) {
    loop {}
}

#[no_mangle]
pub extern "C" fn mattsql_rust_abi_smoke_reset() {
    unsafe {
        *state_ptr() = SmokeRuntimeState::new();
    }
}

#[no_mangle]
pub extern "C" fn mattsql_rust_abi_smoke_runtime() -> MattsqlAbiRuntimeV1 {
    MattsqlAbiRuntimeV1 {
        version: MATTSQL_ABI_RUNTIME_VERSION,
        reserved: 0,
        context: state_ptr().cast::<c_void>(),
        get_capabilities: Some(get_capabilities),
        allocate_pages: Some(allocate_pages),
        free_pages: Some(free_pages),
        submit_io_batch: Some(submit_io_batch),
        poll_io_completions: Some(poll_io_completions),
        yield_fn: Some(yield_runtime),
        monotonic_nanos: Some(monotonic_nanos),
        log: Some(log_message),
        panic: Some(panic_runtime),
    }
}
