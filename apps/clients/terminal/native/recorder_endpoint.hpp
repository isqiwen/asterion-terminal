#pragma once
#include <asterion/foundation/serialization.hpp>
#include <filesystem>

namespace asterion::terminal {

// Where the recording host's read-only interface listens. The Terminal keeps
// it and hands it to lab whenever lab reads recordings, so the address is
// entered once, in Settings. The recorder takes no credential from here: it
// logs in to the broker on its own host.
class RecorderEndpoint
{
public:
    explicit RecorderEndpoint(std::filesystem::path file)
        : file_(std::move(file))
    {}

    // {host, port}, or null when none is saved. An unreadable file is
    // reported with its path, never rewritten.
    Json get() const;
    void save(const std::string& host, unsigned port) const;
    void clear() const;

private:
    std::filesystem::path file_;
};

} // namespace asterion::terminal
