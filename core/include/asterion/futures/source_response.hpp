#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>

namespace asterion {

// Producer receive time and allowlisted source evidence bytes, never requests,
// headers or credentials. The format declares either an exact response body or
// a bundle containing exact original response strings; normalized rows are separate.
struct SourceResponse
{
    std::string format, body;
    std::int64_t received_at_ns = 0;
};

inline constexpr std::size_t max_source_response_bytes = 1024 * 1024;

// Opaque source format identifier in a storage/transport envelope. Providers
// validate supported formats and their contents at the Data boundary.
inline bool valid_source_response_format(std::string_view format)
{
    return !format.empty() && format.size() <= 64
        && format.find_first_not_of(
               "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")
        == std::string_view::npos;
}

} // namespace asterion
