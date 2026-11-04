#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace asterion {

// Thread-local diagnostic context only; never used for authorization or replay.
// The supplied string must outlive this scope. Empty clears inherited context.
class TraceScope final
{
public:
    explicit TraceScope(std::string_view trace) noexcept;
    ~TraceScope();
    TraceScope(const TraceScope&) = delete;
    TraceScope& operator=(const TraceScope&) = delete;

private:
    std::string_view previous_;
};

std::string_view current_trace_id() noexcept;
// Links a request about to be sent, by its id, to the current trace in the log.
void log_request(std::uint64_t id);

} // namespace asterion
