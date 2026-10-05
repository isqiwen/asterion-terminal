#pragma once
#include <string>
#include <string_view>
namespace asterion {
// Thread-local diagnostic context only; never used for authorization or replay.
// The supplied string must outlive this scope. Empty clears inherited context.
class TraceScope final {
public:
  explicit TraceScope(std::string_view trace) noexcept;
  ~TraceScope();
  TraceScope(const TraceScope&) = delete;
  TraceScope& operator=(const TraceScope&) = delete;

private:
  std::string_view previous_;
};
std::string_view current_trace_id() noexcept;
// A fresh process-unique request identity, with a payload-free link to the
// current command/request in requests_YYYY-MM-DD.log when tracing is active.
std::string next_correlation_id();
} // namespace asterion
