#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
namespace asterion {
// Producer receive time and the exact allowlisted response bytes, never a
// request, headers, credentials or a reconstructed JSON object.
struct SourceResponse {
  std::string format, body;
  std::int64_t received_at_ns = 0;
};
inline constexpr std::size_t max_source_response_bytes = 1024 * 1024;
} // namespace asterion
