#![no_std]

mod abi_runtime_types;

use abi_runtime_types::*;
use core::mem::{align_of, offset_of, size_of};

const _: [(); 1] = [(); MATTSQL_ABI_RUNTIME_VERSION as usize];

const _: [(); 24] = [(); size_of::<MattsqlAbiStatus>()];
const _: [(); 8] = [(); align_of::<MattsqlAbiStatus>()];
const _: [(); 0] = [(); offset_of!(MattsqlAbiStatus, code)];
const _: [(); 4] = [(); offset_of!(MattsqlAbiStatus, reserved)];
const _: [(); 8] = [(); offset_of!(MattsqlAbiStatus, message)];
const _: [(); 16] = [(); offset_of!(MattsqlAbiStatus, message_length)];

const _: [(); 48] = [(); size_of::<MattsqlAbiRuntimeCapabilities>()];
const _: [(); 8] = [(); align_of::<MattsqlAbiRuntimeCapabilities>()];
const _: [(); 0] = [(); offset_of!(MattsqlAbiRuntimeCapabilities, page_size)];
const _: [(); 8] = [(); offset_of!(MattsqlAbiRuntimeCapabilities, block_size)];
const _: [(); 16] =
    [(); offset_of!(MattsqlAbiRuntimeCapabilities, max_io_request_size)];
const _: [(); 24] =
    [(); offset_of!(MattsqlAbiRuntimeCapabilities, max_io_batch_size)];
const _: [(); 32] =
    [(); offset_of!(MattsqlAbiRuntimeCapabilities, max_outstanding_io)];
const _: [(); 40] =
    [(); offset_of!(MattsqlAbiRuntimeCapabilities, supports_async_io)];
const _: [(); 41] =
    [(); offset_of!(MattsqlAbiRuntimeCapabilities, supports_flush)];
const _: [(); 42] =
    [(); offset_of!(MattsqlAbiRuntimeCapabilities, supports_barriers)];
const _: [(); 43] =
    [(); offset_of!(MattsqlAbiRuntimeCapabilities, supports_physical_addresses)];
const _: [(); 44] =
    [(); offset_of!(MattsqlAbiRuntimeCapabilities, supports_dma_memory)];
const _: [(); 45] = [(); offset_of!(MattsqlAbiRuntimeCapabilities, reserved)];

const _: [(); 48] = [(); size_of::<MattsqlAbiPageAllocation>()];
const _: [(); 8] = [(); align_of::<MattsqlAbiPageAllocation>()];
const _: [(); 0] = [(); offset_of!(MattsqlAbiPageAllocation, data)];
const _: [(); 8] = [(); offset_of!(MattsqlAbiPageAllocation, page_count)];
const _: [(); 16] = [(); offset_of!(MattsqlAbiPageAllocation, page_size)];
const _: [(); 24] = [(); offset_of!(MattsqlAbiPageAllocation, alignment)];
const _: [(); 32] = [(); offset_of!(MattsqlAbiPageAllocation, flags)];
const _: [(); 36] = [(); offset_of!(MattsqlAbiPageAllocation, reserved)];
const _: [(); 40] = [(); offset_of!(MattsqlAbiPageAllocation, physical_address)];

const _: [(); 56] = [(); size_of::<MattsqlAbiIoRequest>()];
const _: [(); 8] = [(); align_of::<MattsqlAbiIoRequest>()];
const _: [(); 0] = [(); offset_of!(MattsqlAbiIoRequest, id)];
const _: [(); 8] = [(); offset_of!(MattsqlAbiIoRequest, operation)];
const _: [(); 12] = [(); offset_of!(MattsqlAbiIoRequest, flags)];
const _: [(); 16] = [(); offset_of!(MattsqlAbiIoRequest, offset)];
const _: [(); 24] = [(); offset_of!(MattsqlAbiIoRequest, buffer)];
const _: [(); 32] = [(); offset_of!(MattsqlAbiIoRequest, buffer_length)];
const _: [(); 40] = [(); offset_of!(MattsqlAbiIoRequest, length)];
const _: [(); 48] = [(); offset_of!(MattsqlAbiIoRequest, user_data)];

const _: [(); 48] = [(); size_of::<MattsqlAbiIoCompletion>()];
const _: [(); 8] = [(); align_of::<MattsqlAbiIoCompletion>()];
const _: [(); 0] = [(); offset_of!(MattsqlAbiIoCompletion, id)];
const _: [(); 8] = [(); offset_of!(MattsqlAbiIoCompletion, status)];
const _: [(); 32] = [(); offset_of!(MattsqlAbiIoCompletion, bytes_transferred)];
const _: [(); 40] = [(); offset_of!(MattsqlAbiIoCompletion, user_data)];

const _: [(); 16] = [(); size_of::<MattsqlAbiIoSubmissionResult>()];
const _: [(); 8] = [(); align_of::<MattsqlAbiIoSubmissionResult>()];
const _: [(); 0] = [(); offset_of!(MattsqlAbiIoSubmissionResult, submitted_count)];
const _: [(); 8] =
    [(); offset_of!(MattsqlAbiIoSubmissionResult, first_request_id)];

const _: [(); 88] = [(); size_of::<MattsqlAbiRuntimeV1>()];
const _: [(); 8] = [(); align_of::<MattsqlAbiRuntimeV1>()];
const _: [(); 0] = [(); offset_of!(MattsqlAbiRuntimeV1, version)];
const _: [(); 4] = [(); offset_of!(MattsqlAbiRuntimeV1, reserved)];
const _: [(); 8] = [(); offset_of!(MattsqlAbiRuntimeV1, context)];
const _: [(); 16] = [(); offset_of!(MattsqlAbiRuntimeV1, get_capabilities)];
const _: [(); 24] = [(); offset_of!(MattsqlAbiRuntimeV1, allocate_pages)];
const _: [(); 32] = [(); offset_of!(MattsqlAbiRuntimeV1, free_pages)];
const _: [(); 40] = [(); offset_of!(MattsqlAbiRuntimeV1, submit_io_batch)];
const _: [(); 48] = [(); offset_of!(MattsqlAbiRuntimeV1, poll_io_completions)];
const _: [(); 56] = [(); offset_of!(MattsqlAbiRuntimeV1, yield_fn)];
const _: [(); 64] = [(); offset_of!(MattsqlAbiRuntimeV1, monotonic_nanos)];
const _: [(); 72] = [(); offset_of!(MattsqlAbiRuntimeV1, log)];
const _: [(); 80] = [(); offset_of!(MattsqlAbiRuntimeV1, panic)];
