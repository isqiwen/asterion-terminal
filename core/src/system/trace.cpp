#include <asterion/system/trace.hpp>
#include <asterion/system/log.hpp>

namespace asterion {
namespace {

thread_local std::string_view trace_id;

} // namespace

TraceScope::TraceScope(std::string_view trace) noexcept
    : previous_(trace_id)
{
    trace_id = trace;
}

TraceScope::~TraceScope()
{
    trace_id = previous_;
}

std::string_view current_trace_id() noexcept
{
    return trace_id;
}

void log_request(std::uint64_t id)
{
    if (!trace_id.empty())
        log::info("rpc started: trace_id={} correlation_id={}", trace_id, id);
}

} // namespace asterion
