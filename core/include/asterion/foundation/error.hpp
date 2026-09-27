#pragma once
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
namespace asterion {
enum class ErrorCode {
  invalid_request,
  unavailable,
  conflict,
  permission_denied,
  resource_exhausted,
  cancelled,
  internal_error
};
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
  case ErrorCode::internal_error:
    return "internal_error";
  }
  return "internal_error";
}
class Error final : public std::runtime_error {
public:
  Error(ErrorCode code, std::string message)
      : std::runtime_error(std::move(message)), code_(code) {}
  ErrorCode code() const noexcept { return code_; }

private:
  ErrorCode code_;
};
} // namespace asterion
