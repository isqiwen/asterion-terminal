#pragma once
#include <asterion/resource.pb.h>
#include <asterion/kernel/process/artifact.hpp>
#include <filesystem>
#include <stop_token>
#include <vector>

namespace asterion::programs {

// Immutable native program bytes. The caller owns the domain writer and
// serializes publication; there is no process supervisor or execution here.
class Files
{
public:
    struct Entry
    {
        std::string digest;
        std::uintmax_t bytes;
        HostPlatform platform;
    };

    Files(std::filesystem::path directory, resource::proto::Owner);
    Entry preserve(
        const std::filesystem::path& source,
        const std::string& expected,
        std::stop_token = {});
    Entry verify(const std::string& digest, std::stop_token = {}) const;
    // Metadata only: callers verify bytes before execution or a complete backup.
    std::vector<Entry> catalogue(const std::string& after = {}, unsigned limit = 100) const;
    std::filesystem::path path(const std::string& digest) const;

private:
    std::filesystem::path directory_;
    resource::proto::Owner owner_;
    bool authority() const;
    Entry metadata(const std::string&) const;
};

} // namespace asterion::programs
