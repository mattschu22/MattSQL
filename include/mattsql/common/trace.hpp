#pragma once

#include <chrono>

namespace mattsql {

using TraceClock = std::chrono::steady_clock;
using TraceSink = void (*)(const char *name, const char *category,
                           TraceClock::time_point start,
                           TraceClock::time_point end, void *context);

inline thread_local TraceSink current_trace_sink = nullptr;
inline thread_local void *current_trace_context = nullptr;

inline void SetTraceSink(TraceSink sink, void *context) {
  current_trace_sink = sink;
  current_trace_context = context;
}

inline void ClearTraceSink() {
  current_trace_sink = nullptr;
  current_trace_context = nullptr;
}

class ScopedTrace final {
public:
  ScopedTrace(const char *name, const char *category)
      : name_(name), category_(category), sink_(current_trace_sink),
        context_(current_trace_context) {
    if (sink_ != nullptr) {
      start_ = TraceClock::now();
    }
  }

  ScopedTrace(const ScopedTrace &) = delete;
  ScopedTrace &operator=(const ScopedTrace &) = delete;

  ~ScopedTrace() {
    if (sink_ != nullptr) {
      sink_(name_, category_, start_, TraceClock::now(), context_);
    }
  }

private:
  const char *name_ = "";
  const char *category_ = "";
  TraceClock::time_point start_;
  TraceSink sink_ = nullptr;
  void *context_ = nullptr;
};

} // namespace mattsql
