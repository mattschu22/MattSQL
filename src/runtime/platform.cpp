#include "mattsql/runtime/platform.hpp"

#include "mattsql/common/result_utils.hpp"

#include <span>
#include <utility>

namespace mattsql {

PlatformRuntime::~PlatformRuntime() = default;

Result<RuntimePageAllocation>
PlatformRuntime::AllocatePages(std::size_t page_count) {
  return AllocatePages(page_count, kDefaultPageSize, kRuntimeMemoryZeroed);
}

Result<IoRequestId> PlatformRuntime::SubmitIo(const IoRequest &request) {
  const auto submission = SubmitIoBatch(std::span<const IoRequest>(&request, 1));
  if (!status_ok(submission.status) || !submission.value.has_value()) {
    return error_result<IoRequestId>(submission.status);
  }
  return ok_result(submission.value->first_request_id);
}

Result<IoCompletion> PlatformRuntime::PollIoCompletion() {
  IoCompletion completion;
  const auto count = PollIoCompletions(std::span<IoCompletion>(&completion, 1));
  if (!status_ok(count.status) || !count.value.has_value()) {
    return error_result<IoCompletion>(count.status);
  }
  if (*count.value == 0) {
    return error_result<IoCompletion>(ErrorCode::NotFound,
                                      "no I/O completions are ready");
  }
  return ok_result(std::move(completion));
}

} // namespace mattsql
