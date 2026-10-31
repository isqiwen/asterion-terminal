#pragma once
#include <asterion/base/serialization.hpp>
#include <filesystem>

namespace asterion::terminal {

// Which directory holds the user's strategies: a Git work tree of their own
// (decision D5). The Terminal keeps it and names it in every request to lab
// that reads strategies; lab checks it is a work tree when it reads one.
class StrategyRepository
{
public:
    explicit StrategyRepository(std::filesystem::path file)
        : file_(std::move(file))
    {}

    // {path}, or null when none is chosen. An unreadable file is reported
    // with its path, never rewritten.
    Json get() const;
    void save(const std::string& path) const;
    void clear() const;

private:
    std::filesystem::path file_;
};

} // namespace asterion::terminal
