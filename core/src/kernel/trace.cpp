#include <asterion/kernel/trace.hpp>
#include <asterion/kernel/logger.hpp>
#include <asterion/kernel/process/child.hpp>
namespace asterion {
namespace {
thread_local std::string_view trace_id;
}
TraceScope::TraceScope(std::string_view trace) noexcept : previous_(trace_id) {
  trace_id = trace;
}
TraceScope::~TraceScope() {
  trace_id = previous_;
}
std::string_view current_trace_id() noexcept {
  return trace_id;
}
std::string next_correlation_id() {
  static IdSequence ids{unique_process_id()};
  auto id = ids.next();
  if (!trace_id.empty())
    log_identity_event("requests", "rpc.started", {{"correlation_id", id}});
  return id;
}
} // namespace asterion
