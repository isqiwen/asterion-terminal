#pragma once
#include <asterion/base/serialization.hpp>
#include <filesystem>

namespace asterion::terminal {

// A service's saved address and, for Market, this Terminal's watchlist. No broker credentials.
class SavedEndpoint
{
public:
    explicit SavedEndpoint(std::filesystem::path file)
        : file_(std::move(file))
    {}

    // {host, port, fingerprint, instruments}, or null when none is saved. An unreadable file is
    // reported with its path, never rewritten.
    Json get() const;
    void save(const std::string& host, unsigned port, const std::string& fingerprint) const;
    void clear() const;
    void select(const Json& instruments) const;

private:
    std::filesystem::path file_;
};

} // namespace asterion::terminal
