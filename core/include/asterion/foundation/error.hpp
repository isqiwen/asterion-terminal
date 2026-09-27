#pragma once
#include <array>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
namespace asterion {
// Stable, language-neutral wire codes. User interfaces localize by code and
// show the English diagnostic message only as detail.
enum class ErrorCode {
  invalid_request,
  unavailable,
  conflict,
  permission_denied,
  resource_exhausted,
  cancelled,
  not_found,
  recovery_required,
  operation_failed,
  internal_error
};
inline constexpr std::array<ErrorCode, 10> error_codes{
    ErrorCode::invalid_request,   ErrorCode::unavailable,        ErrorCode::conflict,
    ErrorCode::permission_denied, ErrorCode::resource_exhausted, ErrorCode::cancelled,
    ErrorCode::not_found,         ErrorCode::recovery_required,  ErrorCode::operation_failed,
    ErrorCode::internal_error};
constexpr std::string_view error_name(ErrorCode code) noexcept {
  switch (code) {
  case ErrorCode::invalid_request:
    return "invalid_request";
  case ErrorCode::unavailable:
    return "unavailable";
  case ErrorCode::conflict:
    return "conflict";
  case ErrorCode::permission_denied:
    return "permission_denied";
  case ErrorCode::resource_exhausted:
    return "resource_exhausted";
  case ErrorCode::cancelled:
    return "cancelled";
  case ErrorCode::not_found:
    return "not_found";
  case ErrorCode::recovery_required:
    return "recovery_required";
  case ErrorCode::operation_failed:
    return "operation_failed";
  case ErrorCode::internal_error:
    return "internal_error";
  }
  return "internal_error";
}
constexpr std::optional<ErrorCode> parse_error_code(std::string_view name) noexcept {
  for (const auto code : error_codes)
    if (error_name(code) == name)
      return code;
  return std::nullopt;
}
class Error final : public std::runtime_error {
public:
  Error(ErrorCode code, std::string message)
      : std::runtime_error(std::move(message)), code_(code) {}
  ErrorCode code() const noexcept { return code_; }

private:
  ErrorCode code_;
};
// Rebuilds a typed error received from another process. Unknown codes from a
// newer peer degrade to operation_failed instead of being trusted blindly.
[[noreturn]] inline void throw_remote_error(std::string_view code, std::string message) {
  throw Error(parse_error_code(code).value_or(ErrorCode::operation_failed), std::move(message));
}
} // namespace asterion
