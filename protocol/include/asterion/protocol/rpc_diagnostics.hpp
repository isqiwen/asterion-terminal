#pragma once
#include <asterion/kernel/logger.hpp>
#include <initializer_list>
#include <optional>
namespace asterion::protocol {
// Explicit allowlist only: operation from the schema, stable error code and
// caller-selected identifiers. Never serialize a request, response or message.
// A completed handler is not proof that its reply reached the client.
template <class Request, class Response>
void log_rpc_result(const std::string& logger, const Request& request, const Response& response,
                    bool routine = false,
                    std::initializer_list<std::pair<std::string_view, std::string_view>> ids = {},
                    std::optional<std::uint32_t> attempt = {}) noexcept {
  if (routine && !response.has_error())
    return;
  try {
    Json fields{{"success", !response.has_error()}};
    const auto add = [&](std::string_view name, std::string_view value) {
      if (value.empty())
        return;
      // Invalid peer-supplied identifiers are omitted, not echoed as diagnostics.
      try {
        validate_id(value);
      } catch (const Error&) {
        return;
      }
      fields[std::string(name)] = value;
    };
    add("correlation_id", request.correlation_id());
    const auto* operation = request.GetDescriptor()->FindFieldByNumber(request.operation_case());
    fields["operation"] = operation ? operation->name() : "missing";
    if (response.has_error())
      fields["code"] = error_name(
          parse_error_code(response.error().code()).value_or(ErrorCode::operation_failed));
    for (const auto& [name, value] : ids)
      add(name, value);
    if (attempt)
      fields["attempt"] = *attempt;
    log_process_event(logger, response.has_error() ? LogLevel::warning : LogLevel::info,
                      "rpc.completed", std::move(fields));
  } catch (...) {
    static std::atomic<std::uint64_t> failures{0};
    log_process_failure(logger, "trace.failed", ErrorCode::internal_error, ++failures);
  }
}
} // namespace asterion::protocol
