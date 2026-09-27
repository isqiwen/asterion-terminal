#pragma once
#include <asterion/foundation/error.hpp>
#include <atomic>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
namespace asterion {
inline void validate_id(std::string_view value) {
    if (value.empty() || value.size() > 128) throw Error(ErrorCode::invalid_request, "invalid identity length");
    for (const char c : value) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == ':'))
            throw Error(ErrorCode::invalid_request, "invalid identity character");
    }
}
// Unique within this generator. The host supplies a distinct scope for each run.
class IdSequence final {
public:
    explicit IdSequence(std::string scope) : scope_(std::move(scope)) {
        validate_id(scope_);
        if (scope_.size() > 100) throw Error(ErrorCode::invalid_request, "identity scope too long");
    }
    std::string next() {
        auto value = next_.load();
        do {
            if (value == std::numeric_limits<std::uint64_t>::max()) throw Error(ErrorCode::resource_exhausted, "identity sequence exhausted");
        } while (!next_.compare_exchange_weak(value, value + 1));
        return scope_ + ":" + std::to_string(value);
    }
private:
    std::string scope_;
    std::atomic<std::uint64_t> next_{1};
};
}
