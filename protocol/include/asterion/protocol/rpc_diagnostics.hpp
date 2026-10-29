#pragma once
#include <asterion/foundation/error.hpp>
#include <asterion/foundation/id.hpp>
#include <asterion/kernel/log.hpp>
#include <string>
#include <initializer_list>
#include <optional>

namespace asterion::protocol {

// Explicit allowlist only: operation from the schema, stable error code and
// caller-selected identifiers. Never serialize a request, response or message.
// A completed handler is not proof that its reply reached the client.
template <class Request, class Response>
void log_rpc_result(
    const std::string& logger,
    const Request& request,
    const Response& response,
    bool routine = false,
    std::initializer_list<std::pair<std::string_view, std::string_view>> ids = {},
    std::optional<std::uint32_t> attempt = {}) noexcept
{
    if (routine && !response.has_error())
        return;
    try {
        std::string line = "rpc " + std::string(response.has_error() ? "failed" : "completed")
            + ": module=" + logger;
        const auto add = [&](std::string_view name, std::string_view value) {
            if (value.empty())
                return;
            // Invalid peer-supplied identifiers are omitted, not echoed as diagnostics.
            try {
                validate_id(value);
            } catch (const Error&) {
                return;
            }
            line += " " + std::string(name) + "=" + std::string(value);
        };
        const auto* operation =
            request.GetDescriptor()->FindFieldByNumber(request.operation_case());
        line += " operation=" + (operation ? operation->name() : std::string("missing"));
        add("correlation_id", request.correlation_id());
        if (response.has_error())
            line += " code="
                + std::string(error_name(parse_error_code(response.error().code())
                        .value_or(ErrorCode::operation_failed)));
        for (const auto& [name, value] : ids)
            add(name, value);
        if (attempt)
            line += " attempt=" + std::to_string(*attempt);
        log::write(response.has_error() ? log::Level::warning : log::Level::info, line);
    } catch (...) {
        log::write(log::Level::error, "an rpc result could not be logged");
    }
}

} // namespace asterion::protocol
